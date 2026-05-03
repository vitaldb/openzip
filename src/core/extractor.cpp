#include "extractor.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <vector>

#include "path_validator.h"
#include "secure_string.h"

// minizip-ng (vcpkg port: minizip-ng).
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace fs = std::filesystem;

namespace openzip {

namespace {

constexpr int32_t kReadBufSize = 64 * 1024;
constexpr int kMaxConcurrency = 16;
constexpr size_t kParallelMinEntries = 4;

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::string ToUtf8(const fs::path& p) { return ToUtf8(p.wstring()); }

uint64_t FileSize(const fs::path& p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    return ec ? 0 : sz;
}

bool IsDirEntry(const Extractor::Entry& e, const char* raw_name, size_t raw_len) {
    if (!e.name.empty() && (e.name.back() == L'/' || e.name.back() == L'\\')) return true;
    if (raw_len > 0 && (raw_name[raw_len - 1] == '/' || raw_name[raw_len - 1] == '\\')) return true;
    return false;
}

bool CreateDirsRecursive(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return !ec;
}

fs::path MakeUniqueName(const fs::path& dest) {
    if (!fs::exists(dest)) return dest;
    fs::path stem = dest.stem();
    fs::path ext = dest.extension();
    fs::path parent = dest.parent_path();
    for (int i = 1; i < 10000; ++i) {
        wchar_t buf[32];
        swprintf_s(buf, L" (%d)", i);
        fs::path candidate = parent / (stem.wstring() + buf + ext.wstring());
        if (!fs::exists(candidate)) return candidate;
    }
    return dest;
}

// Open the destination file for extraction, atomically detecting conflicts.
//
// CREATE_NEW + GetLastError(ERROR_FILE_EXISTS) is the only race-free way to
// say "create, but only if nobody beat me to it." The previous flow
// (fs::exists → ask user → CreateFileW(CREATE_ALWAYS)) had a window during
// which another process could land a file at `path` that we'd silently
// clobber.
//
// On conflict the caller's UI callback decides Overwrite / Skip / Rename /
// Cancel. Rename loops with MakeUniqueName until either CREATE_NEW succeeds
// or no candidate name fits (in which case we surface it as Cancel rather
// than spinning forever).
//
// path_io is updated with the final extraction path (relevant when the user
// chose Rename). On INVALID_HANDLE_VALUE the out-param `final_action` says
// why: Cancel = abort everything, Skip = skip this entry, Overwrite/Rename =
// I/O error after the user authorized overwrite/rename.
HANDLE OpenForExtractAtomic(fs::path& path_io,
                             Extractor::ProgressCallback& cb,
                             Extractor::ConflictAction& final_action) {
    constexpr int kRenameRetryLimit = 10000;
    for (int attempt = 0; attempt < kRenameRetryLimit; ++attempt) {
        std::wstring lp = MakeLongPath(path_io);
        HANDLE h = ::CreateFileW(lp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            final_action = Extractor::ConflictAction::Overwrite;  // succeeded outright
            return h;
        }
        DWORD err = ::GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) {
            final_action = Extractor::ConflictAction::Cancel;  // genuine I/O failure
            return INVALID_HANDLE_VALUE;
        }

        // Conflict — ask the user (parallel callers serialize via a mutex on the dialog side).
        Extractor::ConflictAction act = cb.OnFileConflict(path_io.wstring());
        if (act == Extractor::ConflictAction::Cancel ||
            act == Extractor::ConflictAction::Skip) {
            final_action = act;
            return INVALID_HANDLE_VALUE;
        }
        if (act == Extractor::ConflictAction::Overwrite) {
            // Honour the user's explicit overwrite decision. CREATE_ALWAYS still
            // races against another process inserting/replacing the file in the
            // tiny window between dialog and create — but the user already said
            // "replace whatever's there", so this matches their intent.
            HANDLE h2 = ::CreateFileW(lp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h2 == INVALID_HANDLE_VALUE) {
                final_action = Extractor::ConflictAction::Cancel;
                return INVALID_HANDLE_VALUE;
            }
            final_action = Extractor::ConflictAction::Overwrite;
            return h2;
        }
        // Rename — pick a fresh candidate and retry CREATE_NEW.
        fs::path renamed = MakeUniqueName(path_io);
        if (renamed == path_io) {
            // No candidate available — give up rather than busy-loop.
            final_action = Extractor::ConflictAction::Cancel;
            return INVALID_HANDLE_VALUE;
        }
        path_io = std::move(renamed);
    }
    final_action = Extractor::ConflictAction::Cancel;
    return INVALID_HANDLE_VALUE;
}

class ZipReaderHandle {
public:
    ZipReaderHandle() { handle_ = mz_zip_reader_create(); }
    ~ZipReaderHandle() {
        if (handle_) {
            mz_zip_reader_close(handle_);
            mz_zip_reader_delete(&handle_);
        }
    }
    void* get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }
    ZipReaderHandle(const ZipReaderHandle&) = delete;
    ZipReaderHandle& operator=(const ZipReaderHandle&) = delete;

private:
    void* handle_ = nullptr;
};

class FileHandle {
public:
    explicit FileHandle(HANDLE h) : h_(h) {}
    ~FileHandle() { if (h_ != INVALID_HANDLE_VALUE) ::CloseHandle(h_); }
    HANDLE get() const { return h_; }
    bool valid() const { return h_ != INVALID_HANDLE_VALUE; }
    void release() { h_ = INVALID_HANDLE_VALUE; }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

// Detect symlink / reparse-point entries from ZIP central-directory metadata.
// We refuse to extract these regardless of host OS:
//   * On Unix, the host byte (high byte of version_madeby) is 3 and external_fa
//     carries Unix mode bits in its high 16 bits. S_IFLNK == 0xA000 in standard
//     stat.h; matched against the file-type mask 0xF000.
//   * On Windows (host byte 0 or 11), external_fa is a DWORD of FILE_ATTRIBUTE_*
//     bits. FILE_ATTRIBUTE_REPARSE_POINT == 0x400 — directory junctions and
//     symbolic links both set this.
//   * minizip-ng populates `linkname` whenever it parses a symlink record from
//     either Unix or Info-ZIP extra fields. A non-empty linkname is a strong
//     signal regardless of host.
// On Windows we never call CreateSymbolicLinkW, so a malicious entry would land
// as a regular file containing the target path — which is harmless on its own
// but combined with future filesystem state could enable confusing extractions.
// Refuse outright; users with a legitimate need for symlinked archives can use
// a different tool.
bool DetectSymlinkOrReparse(const mz_zip_file* fi) {
    if (fi->linkname && fi->linkname[0] != '\0') return true;

    uint8_t host = static_cast<uint8_t>((fi->version_madeby >> 8) & 0xFF);
    uint32_t fa  = fi->external_fa;
    if (host == 3) {
        // Unix host — high 16 bits of external_fa carry the mode.
        uint32_t mode = (fa >> 16) & 0xFFFF;
        if ((mode & 0xF000) == 0xA000) return true;       // S_IFLNK
    } else {
        // Windows / DOS host — low bits are FILE_ATTRIBUTE_*.
        if ((fa & 0x00000400) != 0) return true;           // FILE_ATTRIBUTE_REPARSE_POINT
    }
    return false;
}

Extractor::Entry BuildEntry(const mz_zip_file* fi) {
    Extractor::Entry e;
    bool utf8_flag = (fi->flag & MZ_ZIP_FLAG_UTF8) != 0;
    size_t name_len = fi->filename_size > 0 ? fi->filename_size
                                            : (fi->filename ? std::strlen(fi->filename) : 0);
    DecodedName decoded = DecodeFilename(fi->filename, name_len, utf8_flag);
    e.name = std::move(decoded.text);
    e.decode_source = decoded.source;
    e.uncompressed_size = fi->uncompressed_size > 0 ? static_cast<uint64_t>(fi->uncompressed_size) : 0;
    e.compressed_size = fi->compressed_size > 0 ? static_cast<uint64_t>(fi->compressed_size) : 0;
    // mz_zip_file::modified_date / creation_date are time_t (seconds since 1970 UTC).
    e.modified_time = (fi->modified_date > 0)
                      ? static_cast<std::time_t>(fi->modified_date) : 0;
    e.created_time  = (fi->creation_date > 0)
                      ? static_cast<std::time_t>(fi->creation_date) : 0;
    e.needs_password = (fi->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0;
    e.is_dir = IsDirEntry(e, fi->filename, name_len);
    e.is_symlink = DetectSymlinkOrReparse(fi);
    return e;
}

// Sequential-mode password handling — preserves the v0.1 retry behavior.
int32_t OpenEntryWithPassword(void* reader, const std::wstring& archive_name,
                              const std::wstring& entry_name, bool is_encrypted,
                              Extractor::ProgressCallback& cb,
                              std::string& password_utf8, bool& password_supplied,
                              Extractor::Result& out_result) {
    auto prompt = [&](bool was_wrong) -> bool {
        std::wstring pw = cb.OnPasswordRequired(archive_name, entry_name, was_wrong);
        SecureZeroOnExit<std::wstring> pw_guard(pw);
        if (pw.empty()) {
            out_result = was_wrong ? Extractor::Result::BadPassword
                                   : Extractor::Result::Cancelled;
            return false;
        }
        // Zero the previous password (if any) before overwriting with the new one.
        SecureZero(password_utf8);
        password_utf8 = ToUtf8(pw);
        mz_zip_reader_set_password(reader, password_utf8.c_str());
        password_supplied = true;
        return true;
    };

    if (is_encrypted && !password_supplied) {
        if (cb.ShouldCancel()) {
            out_result = Extractor::Result::Cancelled;
            return MZ_END_OF_LIST;
        }
        if (!prompt(/*was_wrong=*/false)) return MZ_PASSWORD_ERROR;
    }

    while (true) {
        if (cb.ShouldCancel()) {
            out_result = Extractor::Result::Cancelled;
            return MZ_END_OF_LIST;
        }
        int32_t err = mz_zip_reader_entry_open(reader);
        if (err == MZ_OK) return MZ_OK;

        mz_zip_reader_entry_close(reader);

        bool pw_related = is_encrypted ||
                          err == MZ_PASSWORD_ERROR ||
                          err == MZ_CRYPT_ERROR;
        if (!pw_related) {
            out_result = Extractor::Result::CorruptArchive;
            return err;
        }

        if (!prompt(/*was_wrong=*/true)) return err;
    }
}

// ----- v0.1 single-threaded extraction (kept for encrypted archives + tiny ones) -----

Extractor::Result ExtractSequential(const fs::path& zip_path,
                                    const fs::path& target_dir,
                                    std::vector<Extractor::Entry>& entries,
                                    uint64_t total_uncompressed,
                                    Extractor::ProgressCallback& cb,
                                    const std::set<std::wstring>& include_set) {
    using Result = Extractor::Result;

    ZipReaderHandle reader;
    if (!reader) return Result::IoError;
    if (mz_zip_reader_open_file(reader.get(), ToUtf8(zip_path).c_str()) != MZ_OK) {
        return Result::CorruptArchive;
    }

    const std::wstring archive_name = zip_path.filename().wstring();
    std::string password_utf8;
    SecureZeroOnExit<std::string> password_guard(password_utf8);
    bool password_supplied = false;
    uint64_t bytes_done = 0;
    cb.OnBytes(0, total_uncompressed);

    if (!entries.empty()) {
        if (mz_zip_reader_goto_first_entry(reader.get()) != MZ_OK) {
            return Result::CorruptArchive;
        }
    }

    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (cb.ShouldCancel()) return Result::Cancelled;

        const Extractor::Entry& e = entries[idx];

        // Skip entries not in the include filter (when filtering is active).
        if (!include_set.empty() && include_set.find(e.name) == include_set.end()) {
            mz_zip_reader_goto_next_entry(reader.get());
            continue;
        }

        cb.OnEntryStart(e, idx, entries.size());

        if (e.is_symlink) return Result::UnsafeEntry;

        ValidatedPath vp = ValidatePath(e.name, target_dir);
        if (vp.error == PathError::EscapesTarget) return Result::UnsafePath;
        if (vp.error == PathError::AbsolutePath)  return Result::UnsafePath;
        if (vp.error == PathError::InvalidChar)   return Result::UnsafePath;
        if (vp.error == PathError::ReservedName)  return Result::ReservedName;
        if (vp.error == PathError::EmptyPath) {
            mz_zip_reader_goto_next_entry(reader.get());
            continue;
        }

        const fs::path& dest_path = vp.absolute;

        if (e.is_dir) {
            CreateDirsRecursive(dest_path);
            mz_zip_reader_goto_next_entry(reader.get());
            continue;
        }

        if (dest_path.has_parent_path()) {
            if (!CreateDirsRecursive(dest_path.parent_path())) return Result::IoError;
        }

        Result subresult = Result::CorruptArchive;
        int32_t open_err = OpenEntryWithPassword(reader.get(), archive_name, e.name,
                                                 e.needs_password, cb,
                                                 password_utf8, password_supplied,
                                                 subresult);
        if (open_err != MZ_OK) return subresult;

        // Atomic create-or-conflict — closes the TOCTOU window between the
        // existence check and the actual create.
        fs::path final_path = dest_path;
        Extractor::ConflictAction conflict_outcome = Extractor::ConflictAction::Overwrite;
        FileHandle out(OpenForExtractAtomic(final_path, cb, conflict_outcome));
        if (!out.valid()) {
            mz_zip_reader_entry_close(reader.get());
            if (conflict_outcome == Extractor::ConflictAction::Skip) {
                bytes_done += e.uncompressed_size;
                cb.OnBytes(bytes_done, total_uncompressed);
                mz_zip_reader_goto_next_entry(reader.get());
                continue;
            }
            if (conflict_outcome == Extractor::ConflictAction::Cancel) return Result::Cancelled;
            return Result::IoError;
        }
        std::wstring dest_w = MakeLongPath(final_path);

        std::array<uint8_t, kReadBufSize> buf{};
        bool cancelled = false, ioerror = false, corrupt = false, badpw = false;
        while (true) {
            if (cb.ShouldCancel()) { cancelled = true; break; }
            int32_t n = mz_zip_reader_entry_read(reader.get(), buf.data(),
                                                 static_cast<int32_t>(buf.size()));
            if (n < 0) {
                if (n == MZ_PASSWORD_ERROR || n == MZ_CRYPT_ERROR) badpw = true;
                else corrupt = true;
                break;
            }
            if (n == 0) break;
            DWORD written = 0;
            if (!::WriteFile(out.get(), buf.data(), static_cast<DWORD>(n), &written, nullptr) ||
                written != static_cast<DWORD>(n)) {
                ioerror = true; break;
            }
            bytes_done += static_cast<uint64_t>(n);
            cb.OnBytes(bytes_done, total_uncompressed);
        }

        ::CloseHandle(out.get());
        out.release();
        mz_zip_reader_entry_close(reader.get());

        if (cancelled || ioerror || corrupt || badpw) {
            ::DeleteFileW(dest_w.c_str());
            if (cancelled) return Result::Cancelled;
            if (ioerror) return Result::IoError;
            if (badpw) return Result::BadPassword;
            return Result::CorruptArchive;
        }

        mz_zip_reader_goto_next_entry(reader.get());
    }

    return Result::Success;
}

// ----- v0.2 parallel extraction -----

Extractor::Result ExtractParallel(const fs::path& zip_path,
                                  const fs::path& target_dir,
                                  std::vector<Extractor::Entry>& entries,
                                  uint64_t total_uncompressed,
                                  Extractor::ProgressCallback& cb,
                                  int concurrency,
                                  const std::set<std::wstring>& include_set) {
    using Result = Extractor::Result;

    std::atomic<size_t> next_index{0};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<bool> stop{false};
    std::atomic<int> first_err{static_cast<int>(Result::Success)};

    auto fail = [&](Result r) {
        if (!stop.exchange(true)) {
            first_err.store(static_cast<int>(r));
        }
    };

    cb.OnBytes(0, total_uncompressed);

    const std::string zip_path_utf8 = ToUtf8(zip_path);

    auto worker_proc = [&]() {
        ZipReaderHandle reader;
        if (!reader) { fail(Result::IoError); return; }
        if (mz_zip_reader_open_file(reader.get(), zip_path_utf8.c_str()) != MZ_OK) {
            fail(Result::CorruptArchive);
            return;
        }
        if (mz_zip_reader_goto_first_entry(reader.get()) != MZ_OK) {
            // Empty archive or read failure — let other workers exit cleanly.
            return;
        }
        size_t cursor = 0;

        while (!stop.load(std::memory_order_acquire)) {
            if (cb.ShouldCancel()) { fail(Result::Cancelled); return; }

            size_t claim = next_index.fetch_add(1, std::memory_order_relaxed);
            if (claim >= entries.size()) return;

            // Advance reader cursor to the claimed entry.
            while (cursor < claim) {
                if (mz_zip_reader_goto_next_entry(reader.get()) != MZ_OK) {
                    fail(Result::CorruptArchive);
                    return;
                }
                ++cursor;
            }

            const Extractor::Entry& e = entries[claim];

            // Skip entries not in the include filter (when filtering is active).
            if (!include_set.empty() && include_set.find(e.name) == include_set.end()) {
                continue;
            }

            cb.OnEntryStart(e, claim, entries.size());

            if (e.is_symlink) { fail(Result::UnsafeEntry); return; }

            ValidatedPath vp = ValidatePath(e.name, target_dir);
            if (vp.error == PathError::EscapesTarget) { fail(Result::UnsafePath); return; }
            if (vp.error == PathError::AbsolutePath)  { fail(Result::UnsafePath); return; }
            if (vp.error == PathError::InvalidChar)   { fail(Result::UnsafePath); return; }
            if (vp.error == PathError::ReservedName)  { fail(Result::ReservedName); return; }
            if (vp.error == PathError::EmptyPath)     continue;

            const fs::path& dest_path = vp.absolute;

            if (e.is_dir) {
                CreateDirsRecursive(dest_path);
                continue;
            }

            if (dest_path.has_parent_path()) {
                if (!CreateDirsRecursive(dest_path.parent_path())) {
                    fail(Result::IoError);
                    return;
                }
            }

            // Encrypted entries are routed to the sequential path by the
            // dispatcher, so a plain entry_open is sufficient here.
            int32_t open_err = mz_zip_reader_entry_open(reader.get());
            if (open_err != MZ_OK) {
                mz_zip_reader_entry_close(reader.get());
                fail(Result::CorruptArchive);
                return;
            }

            // Atomic create-or-conflict. Multiple workers may race on the same
            // destination name in degenerate archives; CREATE_NEW guarantees
            // only one wins, the loser falls into the conflict callback (which
            // is itself mutex-serialized in CExtractDialog).
            fs::path final_path = dest_path;
            Extractor::ConflictAction conflict_outcome = Extractor::ConflictAction::Overwrite;
            FileHandle out(OpenForExtractAtomic(final_path, cb, conflict_outcome));
            if (!out.valid()) {
                mz_zip_reader_entry_close(reader.get());
                if (conflict_outcome == Extractor::ConflictAction::Skip) {
                    bytes_done.fetch_add(e.uncompressed_size, std::memory_order_relaxed);
                    cb.OnBytes(bytes_done.load(std::memory_order_relaxed), total_uncompressed);
                    continue;
                }
                if (conflict_outcome == Extractor::ConflictAction::Cancel) {
                    fail(Result::Cancelled);
                    return;
                }
                fail(Result::IoError);
                return;
            }
            std::wstring dest_w = MakeLongPath(final_path);

            std::array<uint8_t, kReadBufSize> buf{};
            bool ok = true;
            bool cancelled = false;
            while (!stop.load(std::memory_order_acquire)) {
                if (cb.ShouldCancel()) { cancelled = true; ok = false; break; }
                int32_t n = mz_zip_reader_entry_read(reader.get(), buf.data(),
                                                     static_cast<int32_t>(buf.size()));
                if (n < 0) { ok = false; break; }
                if (n == 0) break;
                DWORD written = 0;
                if (!::WriteFile(out.get(), buf.data(), static_cast<DWORD>(n),
                                 &written, nullptr) ||
                    written != static_cast<DWORD>(n)) {
                    ok = false;
                    break;
                }
                uint64_t total = bytes_done.fetch_add(static_cast<uint64_t>(n),
                                                      std::memory_order_relaxed) + n;
                cb.OnBytes(total, total_uncompressed);
            }

            ::CloseHandle(out.get());
            out.release();
            mz_zip_reader_entry_close(reader.get());

            if (!ok) {
                ::DeleteFileW(dest_w.c_str());
                fail(cancelled ? Result::Cancelled : Result::IoError);
                return;
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(concurrency));
    for (int i = 0; i < concurrency; ++i) {
        workers.emplace_back(worker_proc);
    }
    for (auto& t : workers) t.join();

    if (cb.ShouldCancel()) return Result::Cancelled;
    return static_cast<Result>(first_err.load());
}

}  // namespace

const char* ResultName(Extractor::Result r) {
    switch (r) {
        case Extractor::Result::Success:        return "Success";
        case Extractor::Result::Cancelled:      return "Cancelled";
        case Extractor::Result::BadPassword:    return "BadPassword";
        case Extractor::Result::IoError:        return "IoError";
        case Extractor::Result::CorruptArchive: return "CorruptArchive";
        case Extractor::Result::BombRefused:    return "BombRefused";
        case Extractor::Result::UnsafePath:     return "UnsafePath";
        case Extractor::Result::ReservedName:   return "ReservedName";
        case Extractor::Result::UnsafeEntry:    return "UnsafeEntry";
    }
    return "Unknown";
}

std::vector<Extractor::Entry> Extractor::ListEntries(const fs::path& zip_path) {
    std::vector<Entry> out;
    ZipReaderHandle reader;
    if (!reader) return out;
    if (mz_zip_reader_open_file(reader.get(), ToUtf8(zip_path).c_str()) != MZ_OK) return out;

    int32_t err = mz_zip_reader_goto_first_entry(reader.get());
    while (err == MZ_OK) {
        mz_zip_file* fi = nullptr;
        if (mz_zip_reader_entry_get_info(reader.get(), &fi) == MZ_OK && fi) {
            out.push_back(BuildEntry(fi));
        }
        err = mz_zip_reader_goto_next_entry(reader.get());
    }
    return out;
}

Extractor::Result Extractor::Extract(const fs::path& zip_path,
                                     const fs::path& target_dir,
                                     ProgressCallback& cb,
                                     const Options& opts) {
    auto finish = [&](Result r) -> Result { cb.OnComplete(r); return r; };

    if (!fs::exists(zip_path)) return finish(Result::IoError);
    if (!fs::exists(target_dir)) {
        if (!CreateDirsRecursive(target_dir)) return finish(Result::IoError);
    }

    // List entries (single-threaded) for total + bomb check + encryption detection.
    std::vector<Entry> entries;
    uint64_t total_uncompressed = 0;
    uint64_t max_entry_uncompressed = 0;
    bool any_symlink = false;
    {
        ZipReaderHandle reader;
        if (!reader) return finish(Result::IoError);
        if (mz_zip_reader_open_file(reader.get(), ToUtf8(zip_path).c_str()) != MZ_OK) {
            return finish(Result::CorruptArchive);
        }
        int32_t err = mz_zip_reader_goto_first_entry(reader.get());
        while (err == MZ_OK) {
            mz_zip_file* fi = nullptr;
            if (mz_zip_reader_entry_get_info(reader.get(), &fi) == MZ_OK && fi) {
                Entry e = BuildEntry(fi);
                if (!e.is_dir) {
                    total_uncompressed += e.uncompressed_size;
                    if (e.uncompressed_size > max_entry_uncompressed) {
                        max_entry_uncompressed = e.uncompressed_size;
                    }
                }
                if (e.is_symlink) any_symlink = true;
                entries.push_back(std::move(e));
            }
            err = mz_zip_reader_goto_next_entry(reader.get());
        }
    }

    // Refuse symlink-bearing archives outright — fail closed before any I/O.
    if (any_symlink) return finish(Result::UnsafeEntry);

    // Decompression bomb guard. Trip when ratio is pathological AND either the
    // total or any single entry breaches its threshold. Two thresholds let us
    // catch small-but-deeply-nested bombs (e.g. 42.zip ≈ 42 KiB → 4.5 PiB) as
    // well as large-and-flat ones.
    uint64_t archive_size = FileSize(zip_path);
    if (archive_size > 0) {
        uint64_t ratio = total_uncompressed / archive_size;
        bool ratio_bad = ratio >= kBombRatioLimit;
        bool size_bad  = total_uncompressed     >= kBombSizeLimit ||
                         max_entry_uncompressed >= kBombPerEntryLimit;
        if (ratio_bad && size_bad) {
            return finish(Result::BombRefused);
        }
    }

    // Build the include-filter set for fast membership check inside the
    // sequential/parallel extract loops. Empty set means "extract everything".
    std::set<std::wstring> include_set(opts.include.begin(), opts.include.end());
    if (!include_set.empty()) {
        // Recompute total bytes for accurate progress when filtering.
        uint64_t filtered_total = 0;
        for (const auto& e : entries) {
            if (!e.is_dir && include_set.find(e.name) != include_set.end()) {
                filtered_total += e.uncompressed_size;
            }
        }
        total_uncompressed = filtered_total;
    }

    // Resolve concurrency.
    int concurrency = opts.concurrency;
    if (concurrency <= 0) {
        unsigned hw = std::thread::hardware_concurrency();
        concurrency = static_cast<int>(hw == 0 ? 1u : hw);
    }
    if (concurrency > kMaxConcurrency) concurrency = kMaxConcurrency;
    if (concurrency < 1) concurrency = 1;

    bool any_encrypted = std::any_of(entries.begin(), entries.end(),
                                     [](const Entry& e){ return e.needs_password; });

    // Encrypted archives keep the v0.1 retry-aware sequential path.
    // Tiny archives don't benefit from threads — overhead would dominate.
    bool use_parallel = concurrency > 1 && !any_encrypted &&
                        entries.size() >= kParallelMinEntries;

    Result r = use_parallel
        ? ExtractParallel(zip_path, target_dir, entries, total_uncompressed, cb, concurrency, include_set)
        : ExtractSequential(zip_path, target_dir, entries, total_uncompressed, cb, include_set);

    return finish(r);
}

}  // namespace openzip
