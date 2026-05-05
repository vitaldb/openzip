#include "xz_archive.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwctype>

#include "path_validator.h"

#include <lzma.h>

namespace fs = std::filesystem;

namespace openzip::xz {

namespace {

constexpr size_t kBufSize = 64 * 1024;

// xz magic per spec §2.1.1.1: 0xFD '7' 'z' 'X' 'Z' 0x00.
constexpr uint8_t kXzMagic[6] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};

std::wstring ToLowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

// Strip the xz suffix to derive the inner filename. `.txz` is the analog of
// `.tgz` for tar.xz, so it expands to `.tar`.
std::wstring StripXzSuffix(const std::wstring& filename_lc,
                           const std::wstring& filename_orig) {
    if (EndsWith(filename_lc, L".tar.xz")) {
        return filename_orig.substr(0, filename_orig.size() - 3);  // drop ".xz"
    }
    if (EndsWith(filename_lc, L".txz")) {
        return filename_orig.substr(0, filename_orig.size() - 4) + L".tar";
    }
    if (EndsWith(filename_lc, L".xz")) {
        return filename_orig.substr(0, filename_orig.size() - 3);
    }
    return filename_orig;
}

uint64_t FileSize(const fs::path& p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    return ec ? 0 : sz;
}

fs::path MakeUniqueName(const fs::path& dest) {
    if (!fs::exists(dest)) return dest;
    fs::path stem = dest.stem();
    fs::path ext = dest.extension();
    fs::path parent = dest.parent_path();
    for (int i = 1; i < 10000; ++i) {
        wchar_t buf[32];
        ::swprintf_s(buf, L" (%d)", i);
        fs::path candidate = parent / (stem.wstring() + buf + ext.wstring());
        if (!fs::exists(candidate)) return candidate;
    }
    return dest;
}

// Atomic create-or-conflict — same shape as the gzip helper. Single
// destination so no rename retry loop beyond the user's per-call decision.
HANDLE OpenForExtractAtomic(fs::path& path_io,
                             Extractor::ProgressCallback& cb,
                             Extractor::ConflictAction& final_action) {
    constexpr int kRenameRetryLimit = 10000;
    for (int attempt = 0; attempt < kRenameRetryLimit; ++attempt) {
        std::wstring lp = MakeLongPath(path_io);
        HANDLE h = ::CreateFileW(lp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            final_action = Extractor::ConflictAction::Overwrite;
            return h;
        }
        DWORD err = ::GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) {
            final_action = Extractor::ConflictAction::Cancel;
            return INVALID_HANDLE_VALUE;
        }
        Extractor::ConflictAction act = cb.OnFileConflict(path_io.wstring());
        if (act == Extractor::ConflictAction::Cancel ||
            act == Extractor::ConflictAction::Skip) {
            final_action = act;
            return INVALID_HANDLE_VALUE;
        }
        if (act == Extractor::ConflictAction::Overwrite) {
            HANDLE h2 = ::CreateFileW(lp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h2 == INVALID_HANDLE_VALUE) {
                final_action = Extractor::ConflictAction::Cancel;
                return INVALID_HANDLE_VALUE;
            }
            final_action = Extractor::ConflictAction::Overwrite;
            return h2;
        }
        fs::path renamed = MakeUniqueName(path_io);
        if (renamed == path_io) {
            final_action = Extractor::ConflictAction::Cancel;
            return INVALID_HANDLE_VALUE;
        }
        path_io = std::move(renamed);
    }
    final_action = Extractor::ConflictAction::Cancel;
    return INVALID_HANDLE_VALUE;
}

// RAII over lzma_stream so an early return can't leak the encoder/decoder
// state allocated by liblzma.
struct LzmaStreamGuard {
    lzma_stream* s;
    explicit LzmaStreamGuard(lzma_stream* p) : s(p) {}
    ~LzmaStreamGuard() { if (s) lzma_end(s); }
    LzmaStreamGuard(const LzmaStreamGuard&) = delete;
    LzmaStreamGuard& operator=(const LzmaStreamGuard&) = delete;
};

// Try to recover the uncompressed size from the xz footer's stream index. xz
// stores it explicitly (unlike gzip's wrapped ISIZE) so we can show an
// accurate progress denominator. Best-effort — returns 0 if the file is
// truncated, multi-stream and we can't parse it cheaply, or any I/O fails.
uint64_t ReadUncompressedSize(const fs::path& p) {
    std::wstring lp = MakeLongPath(p);
    HANDLE h = ::CreateFileW(lp.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER total{};
    if (!::GetFileSizeEx(h, &total) || total.QuadPart < 12) {
        ::CloseHandle(h);
        return 0;
    }

    // Stream footer is the last 12 bytes per xz spec §2.1.2.
    LARGE_INTEGER seek{};
    seek.QuadPart = total.QuadPart - 12;
    if (!::SetFilePointerEx(h, seek, nullptr, FILE_BEGIN)) {
        ::CloseHandle(h); return 0;
    }
    uint8_t footer[12] = {};
    DWORD got = 0;
    if (!::ReadFile(h, footer, 12, &got, nullptr) || got != 12) {
        ::CloseHandle(h); return 0;
    }

    lzma_stream_flags flags{};
    if (lzma_stream_footer_decode(&flags, footer) != LZMA_OK) {
        ::CloseHandle(h); return 0;
    }

    // backward_size is stored as (real_size / 4) - 1.
    uint64_t index_size = flags.backward_size;
    if (index_size == 0 || index_size > static_cast<uint64_t>(total.QuadPart) - 12) {
        ::CloseHandle(h); return 0;
    }

    seek.QuadPart = total.QuadPart - 12 - static_cast<LONGLONG>(index_size);
    if (!::SetFilePointerEx(h, seek, nullptr, FILE_BEGIN)) {
        ::CloseHandle(h); return 0;
    }
    std::vector<uint8_t> idx_buf(index_size);
    if (!::ReadFile(h, idx_buf.data(), static_cast<DWORD>(index_size), &got, nullptr) ||
        got != index_size) {
        ::CloseHandle(h); return 0;
    }
    ::CloseHandle(h);

    lzma_index* index = nullptr;
    uint64_t memlimit = UINT64_MAX;
    size_t in_pos = 0;
    if (lzma_index_buffer_decode(&index, &memlimit, nullptr,
                                 idx_buf.data(), &in_pos, idx_buf.size()) != LZMA_OK) {
        if (index) lzma_index_end(index, nullptr);
        return 0;
    }
    uint64_t uncompressed = lzma_index_uncompressed_size(index);
    lzma_index_end(index, nullptr);
    return uncompressed;
}

}  // namespace

bool LooksLikeXz(const fs::path& p) {
    std::wstring lc = ToLowerCopy(p.filename().wstring());
    return EndsWith(lc, L".xz") || EndsWith(lc, L".txz");
}

bool HasXzMagic(const fs::path& p) {
    std::wstring lp = MakeLongPath(p);
    HANDLE h = ::CreateFileW(lp.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t magic[sizeof(kXzMagic)] = {};
    DWORD n = 0;
    bool ok = ::ReadFile(h, magic, sizeof(magic), &n, nullptr) &&
              n == sizeof(magic) &&
              std::memcmp(magic, kXzMagic, sizeof(magic)) == 0;
    ::CloseHandle(h);
    return ok;
}

std::vector<Extractor::Entry> ListEntries(const fs::path& xz_path) {
    std::vector<Extractor::Entry> out;
    if (!fs::exists(xz_path)) return out;

    std::wstring fn = xz_path.filename().wstring();
    std::wstring inner = StripXzSuffix(ToLowerCopy(fn), fn);
    if (inner.empty()) inner = L"file";

    Extractor::Entry e;
    e.name = inner;
    e.decode_source = DecodeSource::Utf8;  // tag for UI; xz has no name field
    e.uncompressed_size = ReadUncompressedSize(xz_path);  // 0 if unknown
    e.compressed_size   = FileSize(xz_path);
    e.is_dir = false;
    e.is_symlink = false;
    e.needs_password = false;
    out.push_back(std::move(e));
    return out;
}

Extractor::Result Extract(const fs::path& xz_path,
                          const fs::path& target_dir,
                          Extractor::ProgressCallback& cb) {
    using Result = Extractor::Result;
    auto finish = [&](Result r) -> Result { cb.OnComplete(r); return r; };

    if (!fs::exists(xz_path)) return finish(Result::IoError);
    {
        std::error_code ec;
        fs::create_directories(target_dir, ec);
        if (ec && !fs::exists(target_dir)) return finish(Result::IoError);
    }

    auto entries = ListEntries(xz_path);
    if (entries.empty()) return finish(Result::CorruptArchive);
    Extractor::Entry& e = entries[0];

    ValidatedPath vp = ValidatePath(e.name, target_dir);
    if (vp.error == PathError::EscapesTarget ||
        vp.error == PathError::AbsolutePath  ||
        vp.error == PathError::InvalidChar) return finish(Result::UnsafePath);
    if (vp.error == PathError::ReservedName) return finish(Result::ReservedName);
    if (vp.error == PathError::EmptyPath)    return finish(Result::CorruptArchive);

    fs::path dest_path = vp.absolute;
    if (dest_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(dest_path.parent_path(), ec);
        if (ec && !fs::exists(dest_path.parent_path())) return finish(Result::IoError);
    }

    uint64_t total = e.uncompressed_size > 0 ? e.uncompressed_size : e.compressed_size;
    cb.OnEntryStart(e, 0, 1);
    cb.OnBytes(0, total);

    fs::path final_path = dest_path;
    Extractor::ConflictAction conflict_outcome = Extractor::ConflictAction::Overwrite;
    HANDLE out_h = OpenForExtractAtomic(final_path, cb, conflict_outcome);
    if (out_h == INVALID_HANDLE_VALUE) {
        if (conflict_outcome == Extractor::ConflictAction::Skip)   return finish(Result::Success);
        if (conflict_outcome == Extractor::ConflictAction::Cancel) return finish(Result::Cancelled);
        return finish(Result::IoError);
    }
    std::wstring dest_w = MakeLongPath(final_path);

    HANDLE in_h = ::CreateFileW(MakeLongPath(xz_path).c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (in_h == INVALID_HANDLE_VALUE) {
        ::CloseHandle(out_h);
        ::DeleteFileW(dest_w.c_str());
        return finish(Result::IoError);
    }

    // Use lzma_stream_decoder so we accept any xz file (single or multi
    // stream) and apply CRC checks. Memlimit ~1 GiB matches `xz -d`'s
    // generous default; bigger archives still legitimately decode while
    // pathological dictionaries are bounded.
    lzma_stream strm = LZMA_STREAM_INIT;
    LzmaStreamGuard sg(&strm);
    if (lzma_stream_decoder(&strm, /*memlimit=*/UINT64_C(1024) << 20,
                            LZMA_CONCATENATED) != LZMA_OK) {
        ::CloseHandle(in_h);
        ::CloseHandle(out_h);
        ::DeleteFileW(dest_w.c_str());
        return finish(Result::IoError);
    }

    std::array<uint8_t, kBufSize> in_buf{};
    std::array<uint8_t, kBufSize> out_buf{};
    strm.next_in = nullptr;
    strm.avail_in = 0;
    strm.next_out = out_buf.data();
    strm.avail_out = out_buf.size();

    uint64_t bytes_done = 0;
    Result r = Result::Success;
    bool eof = false;
    lzma_action action = LZMA_RUN;

    while (true) {
        if (cb.ShouldCancel()) { r = Result::Cancelled; break; }

        if (strm.avail_in == 0 && !eof) {
            DWORD nread = 0;
            if (!::ReadFile(in_h, in_buf.data(), static_cast<DWORD>(in_buf.size()),
                            &nread, nullptr)) {
                r = Result::IoError;
                break;
            }
            if (nread == 0) { eof = true; action = LZMA_FINISH; }
            else { strm.next_in = in_buf.data(); strm.avail_in = nread; }
        }

        lzma_ret ret = lzma_code(&strm, action);

        size_t produced = out_buf.size() - strm.avail_out;
        if (produced > 0) {
            DWORD written = 0;
            if (!::WriteFile(out_h, out_buf.data(), static_cast<DWORD>(produced),
                             &written, nullptr) ||
                written != static_cast<DWORD>(produced)) {
                r = Result::IoError;
                break;
            }
            bytes_done += produced;
            cb.OnBytes(bytes_done, std::max(total, bytes_done));
            strm.next_out = out_buf.data();
            strm.avail_out = out_buf.size();

            // Bomb guard mirrors zip/gzip: ratio-bad AND size-bad.
            if (e.compressed_size > 0) {
                uint64_t ratio = bytes_done / e.compressed_size;
                if (ratio >= Extractor::kBombRatioLimit &&
                    bytes_done >= Extractor::kBombPerEntryLimit) {
                    r = Result::BombRefused;
                    break;
                }
            }
        }

        if (ret == LZMA_STREAM_END) break;
        if (ret != LZMA_OK) {
            r = (ret == LZMA_MEM_ERROR || ret == LZMA_MEMLIMIT_ERROR)
                ? Result::IoError : Result::CorruptArchive;
            break;
        }
    }

    ::CloseHandle(in_h);
    ::CloseHandle(out_h);
    if (r != Result::Success) {
        ::DeleteFileW(dest_w.c_str());
    }
    return finish(r);
}

Compressor::Result Compress(const fs::path& source,
                            const fs::path& output_xz,
                            Compressor::ProgressCallback& cb,
                            const Compressor::Options& opts) {
    using Result = Compressor::Result;

    if (!fs::exists(source)) {
        cb.OnComplete(Result::SourceMissing);
        return Result::SourceMissing;
    }
    if (!fs::is_regular_file(source)) {
        // Folders need a multi-file container; the GUI/CLI surface this case
        // before reaching here.
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }

    fs::path effective_output = output_xz;
    if (fs::exists(output_xz)) {
        auto act = cb.OnOutputExists(output_xz);
        switch (act) {
            case Extractor::ConflictAction::Overwrite: break;
            case Extractor::ConflictAction::Skip:
            case Extractor::ConflictAction::Cancel:
                cb.OnComplete(Result::OutputExists);
                return Result::OutputExists;
            case Extractor::ConflictAction::Rename:
                for (int n = 1; n < 1000; ++n) {
                    wchar_t suffix[32];
                    ::swprintf_s(suffix, L" (%d)", n);
                    fs::path candidate = output_xz;
                    candidate.replace_filename(
                        output_xz.stem().wstring() + suffix + output_xz.extension().wstring());
                    if (!fs::exists(candidate)) { effective_output = candidate; break; }
                }
                break;
        }
    }

    fs::path partial = effective_output;
    partial += L".partial";

    // Map the existing Compressor::Level (0/1/6/9) to liblzma presets (0-9).
    // Store maps to preset 0 (no LZMA2 compression); 9 hits the MAX preset.
    uint32_t preset = 6;
    switch (opts.level) {
        case Compressor::Level::Store:  preset = 0; break;
        case Compressor::Level::Fast:   preset = 1; break;
        case Compressor::Level::Normal: preset = 6; break;
        case Compressor::Level::Max:    preset = 9; break;
    }

    HANDLE in_h = ::CreateFileW(MakeLongPath(source).c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (in_h == INVALID_HANDLE_VALUE) {
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }
    HANDLE out_h = ::CreateFileW(MakeLongPath(partial).c_str(), GENERIC_WRITE, 0,
                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out_h == INVALID_HANDLE_VALUE) {
        ::CloseHandle(in_h);
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }

    lzma_stream strm = LZMA_STREAM_INIT;
    LzmaStreamGuard sg(&strm);
    if (lzma_easy_encoder(&strm, preset, LZMA_CHECK_CRC64) != LZMA_OK) {
        ::CloseHandle(in_h);
        ::CloseHandle(out_h);
        std::error_code ec; fs::remove(partial, ec);
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }

    uint64_t total_bytes = FileSize(source);
    uint64_t bytes_done = 0;
    cb.OnEntryStart(source.filename().wstring(), 0, 1);
    cb.OnBytes(0, total_bytes);

    std::array<uint8_t, kBufSize> in_buf{};
    std::array<uint8_t, kBufSize> out_buf{};
    strm.next_in = nullptr;
    strm.avail_in = 0;
    strm.next_out = out_buf.data();
    strm.avail_out = out_buf.size();

    Result r = Result::Success;
    bool eof = false;
    lzma_action action = LZMA_RUN;

    while (true) {
        if (cb.ShouldCancel()) { r = Result::Cancelled; break; }

        if (strm.avail_in == 0 && !eof) {
            DWORD nread = 0;
            if (!::ReadFile(in_h, in_buf.data(), static_cast<DWORD>(in_buf.size()),
                            &nread, nullptr)) {
                r = Result::IoError;
                break;
            }
            if (nread == 0) { eof = true; action = LZMA_FINISH; }
            else {
                strm.next_in = in_buf.data();
                strm.avail_in = nread;
                bytes_done += nread;
                cb.OnBytes(bytes_done, total_bytes);
            }
        }

        lzma_ret ret = lzma_code(&strm, action);

        size_t produced = out_buf.size() - strm.avail_out;
        if (produced > 0) {
            DWORD written = 0;
            if (!::WriteFile(out_h, out_buf.data(), static_cast<DWORD>(produced),
                             &written, nullptr) ||
                written != static_cast<DWORD>(produced)) {
                r = Result::IoError;
                break;
            }
            strm.next_out = out_buf.data();
            strm.avail_out = out_buf.size();
        }

        if (ret == LZMA_STREAM_END) break;
        if (ret != LZMA_OK) {
            r = Result::IoError;
            break;
        }
    }

    ::CloseHandle(in_h);
    ::CloseHandle(out_h);

    if (r != Result::Success) {
        std::error_code ec; fs::remove(partial, ec);
        cb.OnComplete(r);
        return r;
    }

    std::error_code ec;
    fs::rename(partial, effective_output, ec);
    if (ec) {
        fs::remove(effective_output, ec);
        fs::rename(partial, effective_output, ec);
    }
    if (ec) {
        std::error_code rec; fs::remove(partial, rec);
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }

    cb.OnComplete(Result::Success);
    return Result::Success;
}

}  // namespace openzip::xz
