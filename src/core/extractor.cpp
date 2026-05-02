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
    e.needs_password = (fi->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0;
    e.is_dir = IsDirEntry(e, fi->filename, name_len);
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
        if (pw.empty()) {
            out_result = was_wrong ? Extractor::Result::BadPassword
                                   : Extractor::Result::Cancelled;
            return false;
        }
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

        ValidatedPath vp = ValidatePath(e.name, target_dir);
        if (vp.error == PathError::EscapesTarget) return Result::UnsafePath;
        if (vp.error == PathError::AbsolutePath) return Result::UnsafePath;
        if (vp.error == PathError::ReservedName) return Result::ReservedName;
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

        fs::path final_path = dest_path;
        if (fs::exists(final_path)) {
            Extractor::ConflictAction act = cb.OnFileConflict(final_path.wstring());
            if (act == Extractor::ConflictAction::Cancel) return Result::Cancelled;
            if (act == Extractor::ConflictAction::Skip) {
                bytes_done += e.uncompressed_size;
                cb.OnBytes(bytes_done, total_uncompressed);
                mz_zip_reader_goto_next_entry(reader.get());
                continue;
            }
            if (act == Extractor::ConflictAction::Rename) {
                final_path = MakeUniqueName(final_path);
            }
        }

        Result subresult = Result::CorruptArchive;
        int32_t open_err = OpenEntryWithPassword(reader.get(), archive_name, e.name,
                                                 e.needs_password, cb,
                                                 password_utf8, password_supplied,
                                                 subresult);
        if (open_err != MZ_OK) return subresult;

        std::wstring dest_w = MakeLongPath(final_path);
        FileHandle out(::CreateFileW(dest_w.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!out.valid()) {
            mz_zip_reader_entry_close(reader.get());
            return Result::IoError;
        }

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

            ValidatedPath vp = ValidatePath(e.name, target_dir);
            if (vp.error == PathError::EscapesTarget) { fail(Result::UnsafePath); return; }
            if (vp.error == PathError::AbsolutePath)  { fail(Result::UnsafePath); return; }
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

            fs::path final_path = dest_path;
            if (fs::exists(final_path)) {
                // OnFileConflict on the dialog side is mutex-protected (see
                // CExtractDialog) so concurrent workers serialize cleanly.
                Extractor::ConflictAction act = cb.OnFileConflict(final_path.wstring());
                if (act == Extractor::ConflictAction::Cancel) { fail(Result::Cancelled); return; }
                if (act == Extractor::ConflictAction::Skip) {
                    bytes_done.fetch_add(e.uncompressed_size, std::memory_order_relaxed);
                    cb.OnBytes(bytes_done.load(std::memory_order_relaxed), total_uncompressed);
                    continue;
                }
                if (act == Extractor::ConflictAction::Rename) {
                    final_path = MakeUniqueName(final_path);
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

            std::wstring dest_w = MakeLongPath(final_path);
            FileHandle out(::CreateFileW(dest_w.c_str(), GENERIC_WRITE, 0, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!out.valid()) {
                mz_zip_reader_entry_close(reader.get());
                fail(Result::IoError);
                return;
            }

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
                if (!e.is_dir) total_uncompressed += e.uncompressed_size;
                entries.push_back(std::move(e));
            }
            err = mz_zip_reader_goto_next_entry(reader.get());
        }
    }

    // Decompression bomb guard.
    uint64_t archive_size = FileSize(zip_path);
    if (total_uncompressed > kBombSizeLimit && archive_size > 0 &&
        total_uncompressed / archive_size > kBombRatioLimit) {
        return finish(Result::BombRefused);
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
