#include "compressor.h"

#include <Windows.h>

#include <ctime>
#include <vector>

#include "secure_string.h"

#include <mz.h>
#include <mz_strm.h>
#include <mz_strm_os.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace fs = std::filesystem;

namespace openzip {

namespace {

std::string WideToCodepage(const std::wstring& w, UINT cp) {
    if (w.empty()) return {};
    int len = ::WideCharToMultiByte(cp, 0, w.c_str(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(cp, 0, w.c_str(), static_cast<int>(w.size()),
                          out.data(), len, nullptr, nullptr);
    return out;
}

// Convert a wide path to the multi-byte UTF-8 string minizip-ng expects for file paths.
std::string PathToMz(const fs::path& p) { return WideToCodepage(p.wstring(), CP_UTF8); }

// ---------------------------------------------------------------------------
// FlatEntry: a single file to be added, with its name-in-zip.
// ---------------------------------------------------------------------------
struct FlatEntry {
    fs::path on_disk;
    std::wstring rel_in_zip;   // forward-slash-separated, no leading slash
};

void Flatten(const fs::path& src, std::vector<FlatEntry>& out) {
    if (!fs::exists(src)) return;
    // Fix E: skip top-level symlinks (matches the in-recursion skip below)
    if (fs::is_symlink(src)) return;
    std::wstring base = src.filename().wstring();
    if (fs::is_regular_file(src)) {
        out.push_back({src, base});
        return;
    }
    if (fs::is_directory(src)) {
        for (auto it = fs::recursive_directory_iterator(
                 src, fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it) {
            // Skip symlinks (matches Extractor's symlink refusal)
            if (it->is_symlink()) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file()) continue;
            std::wstring rel = base + L"/";
            std::wstring tail = fs::relative(it->path(), src).wstring();
            for (auto& ch : tail) if (ch == L'\\') ch = L'/';
            rel += tail;
            out.push_back({it->path(), rel});
        }
    }
}

// ---------------------------------------------------------------------------
// Fix A: RAII guards for the writer handle and the partial file.
// ---------------------------------------------------------------------------
struct WriterGuard {
    void* w = nullptr;
    ~WriterGuard() {
        if (w) {
            mz_zip_writer_close(w);
            mz_zip_writer_delete(&w);
        }
    }
    void release() { w = nullptr; }
};

struct PartialGuard {
    fs::path path;
    bool armed = true;
    ~PartialGuard() {
        if (armed) {
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
    void disarm() { armed = false; }
};

// ---------------------------------------------------------------------------
// Write a single entry using mz_zip_writer_entry_open + manual file I/O.
// Used for Task 1.4 (CP949 encoding) and the general per-entry write path.
// Returns MZ_OK on success, or a minizip error code on failure.
// Also accumulates bytes_done and fires cb.OnBytes.
// ---------------------------------------------------------------------------
int32_t WriteEntryManual(void* writer,
                         const FlatEntry& fe,
                         const mz_zip_file& file_info,
                         uint64_t& bytes_done,
                         uint64_t total_bytes,
                         Compressor::ProgressCallback& cb) {
    int32_t add_err = mz_zip_writer_entry_open(writer, const_cast<mz_zip_file*>(&file_info));
    if (add_err != MZ_OK) return add_err;

    HANDLE h = ::CreateFileW(fe.on_disk.wstring().c_str(), GENERIC_READ,
                             FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        mz_zip_writer_entry_close(writer);
        return MZ_OPEN_ERROR;
    }

    std::vector<uint8_t> buf(64 * 1024);
    DWORD nread = 0;
    while (::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &nread, nullptr) && nread > 0) {
        int32_t w = mz_zip_writer_entry_write(writer, buf.data(), static_cast<int32_t>(nread));
        if (w < 0) { add_err = w; break; }
        // Task 1.9: Report byte progress after each write
        bytes_done += static_cast<uint64_t>(nread);
        cb.OnBytes(bytes_done, total_bytes);
        // Fix D: honour cancellation mid-write
        if (cb.ShouldCancel()) { add_err = MZ_END_OF_LIST; break; }
    }
    ::CloseHandle(h);
    mz_zip_writer_entry_close(writer);
    return add_err;
}

}  // namespace

// ---------------------------------------------------------------------------
// Compressor::Compress — full implementation (Tasks 1.1 – 1.9)
// ---------------------------------------------------------------------------
Compressor::Result Compressor::Compress(const std::vector<fs::path>& sources,
                                        const fs::path& output_zip,
                                        ProgressCallback& cb,
                                        const Options& opts) {
    // --- Task 1.8: Output-exists conflict resolution (before opening writer) ---
    fs::path effective_output = output_zip;
    if (fs::exists(output_zip)) {
        auto action = cb.OnOutputExists(output_zip);
        switch (action) {
            case Extractor::ConflictAction::Overwrite:
                // proceed; the final rename will overwrite the existing file
                break;
            case Extractor::ConflictAction::Skip:
            case Extractor::ConflictAction::Cancel:
                cb.OnComplete(Result::OutputExists);
                return Result::OutputExists;
            case Extractor::ConflictAction::Rename:
                for (int n = 1; n < 1000; ++n) {
                    wchar_t suffix[32];
                    ::swprintf_s(suffix, L" (%d)", n);
                    fs::path candidate = output_zip;
                    candidate.replace_filename(
                        output_zip.stem().wstring() + suffix + output_zip.extension().wstring());
                    if (!fs::exists(candidate)) { effective_output = candidate; break; }
                }
                break;
        }
    }

    // Partial file derives from effective_output (Task 1.8)
    fs::path partial = effective_output;
    partial += L".partial";

    // --- Open minizip writer ---
    // Fix A: wrap writer and partial file in RAII guards so they are always
    // cleaned up even if a user callback throws or an I/O error escapes.
    // IMPORTANT: pg is declared before wg so that wg (writer) destructs first,
    // releasing all file handles before pg attempts to delete the partial file.
    PartialGuard pg{partial, true};

    WriterGuard wg;
    wg.w = mz_zip_writer_create();
    if (!wg.w) {
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }

    int32_t err = mz_zip_writer_open_file(wg.w, PathToMz(partial).c_str(), 0, 0);
    if (err != MZ_OK) {
        // wg destructor closes/deletes the writer; pg removes the partial (if created).
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }

    // --- Task 1.6: Password / AES-256 ---
    // Store the UTF-8 password in a string that outlives the writer calls.
    // Wrapped in a guard so it is zeroed regardless of which return path runs.
    std::string pw_utf8;
    SecureZeroOnExit<std::string> pw_guard(pw_utf8);
    if (!opts.password.empty()) {
        pw_utf8 = WideToCodepage(opts.password, CP_UTF8);
        mz_zip_writer_set_password(wg.w, pw_utf8.c_str());
        mz_zip_writer_set_aes(wg.w, 1);  // AES-256
    }

    // --- Task 1.5: Compression level ---
    if (opts.level == Level::Store) {
        mz_zip_writer_set_compress_method(wg.w, MZ_COMPRESS_METHOD_STORE);
        mz_zip_writer_set_compress_level(wg.w, 0);
    } else {
        mz_zip_writer_set_compress_method(wg.w, MZ_COMPRESS_METHOD_DEFLATE);
        mz_zip_writer_set_compress_level(wg.w, static_cast<int16_t>(opts.level));
    }

    // --- Flatten sources (Task 1.3: folder recursion) ---
    std::vector<FlatEntry> flat;
    for (const auto& s : sources) {
        if (!fs::exists(s)) {
            cb.OnComplete(Result::SourceMissing);
            return Result::SourceMissing;
            // wg + pg destructors clean up writer and partial
        }
        Flatten(s, flat);
    }

    // --- Task 1.9: Pre-sum total bytes for progress reporting ---
    uint64_t total_bytes = 0;
    for (const auto& fe : flat) {
        std::error_code sec;
        total_bytes += fs::file_size(fe.on_disk, sec);
    }
    uint64_t bytes_done = 0;

    // Determine encoding: Cp949 requires manual entry_open to set raw filename bytes.
    const bool use_cp949 = (opts.filename_encoding == Encoding::Cp949);

    // Fix C: Both paths now use WriteEntryManual (mz_zip_writer_entry_open) so that:
    //  - AES entries always have compression_method = MZ_COMPRESS_METHOD_AES (99) in metadata
    //  - Per-chunk progress is available on both paths
    //  - Cancellation mid-write (Fix D) works on both paths
    // (The implementer previously reported a BCryptGenRandom hang with entry_open + AES;
    //  this was resolved by ensuring mz_zip_writer_set_aes() is called before the loop.)

    // --- Per-entry write loop ---
    for (size_t i = 0; i < flat.size(); ++i) {
        const auto& fe = flat[i];

        // Task 1.7: Cancel check before each entry
        if (cb.ShouldCancel()) {
            cb.OnComplete(Result::Cancelled);
            return Result::Cancelled;
        }

        cb.OnEntryStart(fe.rel_in_zip, i, flat.size());

        // Re-check after OnEntryStart (callback may set cancel in response to entry index)
        if (cb.ShouldCancel()) {
            cb.OnComplete(Result::Cancelled);
            return Result::Cancelled;
        }

        // Build mz_zip_file for this entry.
        // CP949: raw multibyte filename, no UTF8 flag.
        // UTF-8: UTF-8 filename bytes, UTF8 flag set.
        std::string encoded_name = use_cp949
            ? WideToCodepage(fe.rel_in_zip, 949u)
            : WideToCodepage(fe.rel_in_zip, CP_UTF8);

        mz_zip_file file_info{};
        file_info.filename = encoded_name.c_str();
        file_info.flag = use_cp949 ? 0 : MZ_ZIP_FLAG_UTF8;
        file_info.compression_method = (opts.level == Level::Store)
            ? MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE;
        file_info.zip64 = MZ_ZIP64_AUTO;
        file_info.modified_date = std::time(nullptr);
        // AES: set aes_version so minizip stores the AE-x extra field and
        // sets compression_method = 99 (MZ_COMPRESS_METHOD_AES) in the header.
        if (!opts.password.empty()) {
            file_info.aes_version = MZ_AES_VERSION;
            file_info.flag |= MZ_ZIP_FLAG_ENCRYPTED;
        }

        int32_t add_err = WriteEntryManual(wg.w, fe, file_info, bytes_done, total_bytes, cb);

        if (add_err != MZ_OK) {
            // Distinguish cancellation from genuine I/O error
            if (add_err == MZ_END_OF_LIST && cb.ShouldCancel()) {
                cb.OnComplete(Result::Cancelled);
                return Result::Cancelled;
            }
            cb.OnComplete(Result::IoError);
            return Result::IoError;
        }
    }

    // --- Close writer explicitly (before rename) ---
    err = mz_zip_writer_close(wg.w);
    wg.release();  // prevent double-close in destructor
    if (err != MZ_OK) {
        cb.OnComplete(Result::IoError);
        return Result::IoError;
        // pg destructor removes partial
    }

    // --- Atomic rename: partial → effective_output ---
    std::error_code ec;
    fs::rename(partial, effective_output, ec);
    if (ec) {
        // On Windows, rename fails if target exists (Overwrite case).
        // Remove the existing file and retry.
        fs::remove(effective_output, ec);
        fs::rename(partial, effective_output, ec);
    }
    if (ec) {
        cb.OnComplete(Result::IoError);
        return Result::IoError;
        // pg destructor removes partial
    }

    // Rename succeeded: partial is gone, disarm the guard.
    pg.disarm();

    cb.OnComplete(Result::Success);
    return Result::Success;
}

const char* CompressResultName(Compressor::Result r) {
    switch (r) {
        case Compressor::Result::Success:       return "Success";
        case Compressor::Result::Cancelled:     return "Cancelled";
        case Compressor::Result::IoError:       return "IoError";
        case Compressor::Result::SourceMissing: return "SourceMissing";
        case Compressor::Result::OutputExists:  return "OutputExists";
        case Compressor::Result::BadPassword:   return "BadPassword";
    }
    return "Unknown";
}

}  // namespace openzip
