#include "extractor.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <system_error>

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

// Whether a ZIP entry's name indicates a directory.
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

// Generate a non-conflicting variant: foo.txt → "foo (1).txt", "foo (2).txt", ...
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
    return dest;  // give up — caller may overwrite
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

// Translate the current entry's mz_zip_file into our Entry, decoding the filename.
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

// Try opening the current entry; on password failure, prompt and retry.
// Returns MZ_OK on success; otherwise sets out_result to the appropriate Result.
//
// For encrypted entries, minizip-ng expects the password to be set *before*
// the first entry_open call. We prompt up-front if encryption is flagged but
// no password has been supplied yet.
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

    bool was_wrong = false;
    while (true) {
        if (cb.ShouldCancel()) {
            out_result = Extractor::Result::Cancelled;
            return MZ_END_OF_LIST;
        }
        int32_t err = mz_zip_reader_entry_open(reader);
        if (err == MZ_OK) return MZ_OK;

        // After failure, close any partial reader state before retrying.
        mz_zip_reader_entry_close(reader);

        bool pw_related = is_encrypted ||
                          err == MZ_PASSWORD_ERROR ||
                          err == MZ_CRYPT_ERROR;
        if (!pw_related) {
            out_result = Extractor::Result::CorruptArchive;
            return err;
        }

        if (!prompt(/*was_wrong=*/true)) return err;
        was_wrong = true;
    }
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
                                     ProgressCallback& cb) {
    auto finish = [&](Result r) -> Result { cb.OnComplete(r); return r; };

    if (!fs::exists(zip_path)) return finish(Result::IoError);

    // Ensure target directory exists.
    if (!fs::exists(target_dir)) {
        if (!CreateDirsRecursive(target_dir)) return finish(Result::IoError);
    }

    ZipReaderHandle reader;
    if (!reader) return finish(Result::IoError);

    if (mz_zip_reader_open_file(reader.get(), ToUtf8(zip_path).c_str()) != MZ_OK) {
        return finish(Result::CorruptArchive);
    }

    // Gather all entries first (also lets us compute totals + bomb check).
    std::vector<Entry> entries;
    uint64_t total_uncompressed = 0;
    {
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

    const std::wstring archive_name = zip_path.filename().wstring();

    // Conflict-action remembered across the archive (set by callback returning a non-prompt action
    // is the dialog's responsibility; Extractor just calls OnFileConflict each time).

    std::string password_utf8;
    bool password_supplied = false;

    uint64_t bytes_done = 0;
    cb.OnBytes(0, total_uncompressed);

    if (!entries.empty()) {
        if (mz_zip_reader_goto_first_entry(reader.get()) != MZ_OK) {
            return finish(Result::CorruptArchive);
        }
    }

    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (cb.ShouldCancel()) return finish(Result::Cancelled);

        const Entry& e = entries[idx];
        cb.OnEntryStart(e, idx, entries.size());

        // Validate path.
        ValidatedPath vp = ValidatePath(e.name, target_dir);
        if (vp.error == PathError::EscapesTarget) return finish(Result::UnsafePath);
        if (vp.error == PathError::AbsolutePath) return finish(Result::UnsafePath);
        if (vp.error == PathError::ReservedName) return finish(Result::ReservedName);
        if (vp.error == PathError::EmptyPath) {
            // Skip empty / pointless entry; still advance.
            mz_zip_reader_goto_next_entry(reader.get());
            continue;
        }

        const fs::path& dest_path = vp.absolute;

        if (e.is_dir) {
            CreateDirsRecursive(dest_path);
            mz_zip_reader_goto_next_entry(reader.get());
            continue;
        }

        // Make sure parent dir exists.
        if (dest_path.has_parent_path()) {
            if (!CreateDirsRecursive(dest_path.parent_path())) return finish(Result::IoError);
        }

        // Resolve conflicts.
        fs::path final_path = dest_path;
        if (fs::exists(final_path)) {
            ConflictAction act = cb.OnFileConflict(final_path.wstring());
            if (act == ConflictAction::Cancel) return finish(Result::Cancelled);
            if (act == ConflictAction::Skip) {
                bytes_done += e.uncompressed_size;
                cb.OnBytes(bytes_done, total_uncompressed);
                mz_zip_reader_goto_next_entry(reader.get());
                continue;
            }
            if (act == ConflictAction::Rename) {
                final_path = MakeUniqueName(final_path);
            }
            // Overwrite: just proceed; CREATE_ALWAYS truncates.
        }

        // Open entry (with password retry).
        Result subresult = Result::CorruptArchive;
        int32_t open_err = OpenEntryWithPassword(reader.get(), archive_name, e.name,
                                                 e.needs_password, cb,
                                                 password_utf8, password_supplied,
                                                 subresult);
        if (open_err != MZ_OK) return finish(subresult);

        // Open destination file.
        std::wstring dest_w = MakeLongPath(final_path);
        FileHandle out(::CreateFileW(dest_w.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!out.valid()) {
            mz_zip_reader_entry_close(reader.get());
            return finish(Result::IoError);
        }

        // Read in chunks and write.
        std::array<uint8_t, kReadBufSize> buf{};
        bool cancelled = false;
        bool ioerror = false;
        bool corrupt = false;
        bool badpw = false;
        while (true) {
            if (cb.ShouldCancel()) { cancelled = true; break; }
            int32_t n = mz_zip_reader_entry_read(reader.get(), buf.data(),
                                                 static_cast<int32_t>(buf.size()));
            if (n < 0) {
                if (n == MZ_PASSWORD_ERROR || n == MZ_CRYPT_ERROR) badpw = true;
                else corrupt = true;
                break;
            }
            if (n == 0) break;  // EOF
            DWORD written = 0;
            if (!::WriteFile(out.get(), buf.data(), static_cast<DWORD>(n), &written, nullptr) ||
                written != static_cast<DWORD>(n)) {
                ioerror = true;
                break;
            }
            bytes_done += static_cast<uint64_t>(n);
            cb.OnBytes(bytes_done, total_uncompressed);
        }

        // Close output before potentially deleting.
        ::CloseHandle(out.get());
        out.release();

        mz_zip_reader_entry_close(reader.get());

        if (cancelled || ioerror || corrupt || badpw) {
            ::DeleteFileW(dest_w.c_str());  // remove partial file per plan §9
            if (cancelled) return finish(Result::Cancelled);
            if (ioerror) return finish(Result::IoError);
            if (badpw) return finish(Result::BadPassword);
            return finish(Result::CorruptArchive);
        }

        mz_zip_reader_goto_next_entry(reader.get());
    }

    return finish(Result::Success);
}

}  // namespace openzip
