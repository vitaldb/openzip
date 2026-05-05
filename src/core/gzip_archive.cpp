#include "gzip_archive.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "path_validator.h"

#include <zlib.h>

namespace fs = std::filesystem;

namespace openzip::gzip {

namespace {

constexpr int kReadBufSize = 64 * 1024;

std::wstring ToLowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

// Strip the trailing `.gz` and friends to derive the inner filename when the
// gzip header has no FNAME field. `.tgz` / `.taz` expand to `.tar`.
std::wstring StripGzSuffix(const std::wstring& filename_lc,
                           const std::wstring& filename_orig) {
    if (EndsWith(filename_lc, L".tar.gz")) {
        return filename_orig.substr(0, filename_orig.size() - 3);  // drop ".gz"
    }
    if (EndsWith(filename_lc, L".tgz") || EndsWith(filename_lc, L".taz")) {
        return filename_orig.substr(0, filename_orig.size() - 4) + L".tar";
    }
    if (EndsWith(filename_lc, L".gz")) {
        return filename_orig.substr(0, filename_orig.size() - 3);
    }
    return filename_orig;  // no recognised suffix — keep as-is
}

uint64_t FileSize(const fs::path& p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    return ec ? 0 : sz;
}

// Read the FNAME field (NUL-terminated, latin-1 per RFC 1952) and the ISIZE
// trailer (last 4 bytes, uncompressed-size mod 2^32) without holding the file
// open afterwards. Both are best-effort — callers tolerate missing values.
struct GzMeta {
    bool        ok = false;
    std::string fname;          // empty if FNAME flag not set
    uint32_t    isize_mod32 = 0;
};

GzMeta ReadGzMeta(const fs::path& p) {
    GzMeta meta;
    std::wstring lp = MakeLongPath(p);
    HANDLE h = ::CreateFileW(lp.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return meta;

    auto close_h = [&] { ::CloseHandle(h); };

    uint8_t hdr[10] = {};
    DWORD n = 0;
    if (!::ReadFile(h, hdr, sizeof(hdr), &n, nullptr) || n != sizeof(hdr) ||
        hdr[0] != 0x1f || hdr[1] != 0x8b) {
        close_h();
        return meta;
    }
    uint8_t flg = hdr[3];

    // Skip FEXTRA if present (XLEN bytes — little-endian uint16_t length).
    if (flg & 0x04) {
        uint8_t xlenb[2] = {};
        if (!::ReadFile(h, xlenb, 2, &n, nullptr) || n != 2) { close_h(); return meta; }
        uint32_t xlen = static_cast<uint32_t>(xlenb[0]) |
                        (static_cast<uint32_t>(xlenb[1]) << 8);
        LARGE_INTEGER move{};
        move.QuadPart = static_cast<LONGLONG>(xlen);
        if (!::SetFilePointerEx(h, move, nullptr, FILE_CURRENT)) { close_h(); return meta; }
    }

    // FNAME: NUL-terminated original filename. Read byte-by-byte, capped at 1
    // KiB so a malformed header can't drag us into a giant allocation.
    if (flg & 0x08) {
        std::string fname;
        char ch = 0;
        for (size_t i = 0; i < 1024; ++i) {
            if (!::ReadFile(h, &ch, 1, &n, nullptr) || n != 1) { close_h(); return meta; }
            if (ch == '\0') break;
            fname.push_back(ch);
        }
        meta.fname = std::move(fname);
    }

    // Read ISIZE from the last 4 bytes — uncompressed size modulo 2^32. Files
    // larger than 4 GiB have a wrapped ISIZE; treat that as "size unknown" by
    // leaving the field at 0 once we observe the wrap (ISIZE alone can't tell
    // us if the file is 1 GiB or 5 GiB, so reporting it as unknown is safer
    // for the bomb guard and progress display).
    LARGE_INTEGER sz{};
    if (::GetFileSizeEx(h, &sz) && sz.QuadPart >= 4) {
        LARGE_INTEGER tail{};
        tail.QuadPart = sz.QuadPart - 4;
        if (::SetFilePointerEx(h, tail, nullptr, FILE_BEGIN)) {
            uint8_t isize_bytes[4] = {};
            if (::ReadFile(h, isize_bytes, 4, &n, nullptr) && n == 4) {
                meta.isize_mod32 =  static_cast<uint32_t>(isize_bytes[0])        |
                                   (static_cast<uint32_t>(isize_bytes[1]) <<  8) |
                                   (static_cast<uint32_t>(isize_bytes[2]) << 16) |
                                   (static_cast<uint32_t>(isize_bytes[3]) << 24);
            }
        }
    }

    close_h();
    meta.ok = true;
    return meta;
}

// Convert a (latin-1ish) FNAME byte string to wide. Validate that the result
// is a clean filename component — no path separators, no NUL — to keep header
// content from steering extraction outside the target directory.
std::wstring DecodeAndValidateFname(const std::string& fname_bytes) {
    std::wstring w;
    w.reserve(fname_bytes.size());
    for (unsigned char b : fname_bytes) {
        if (b == 0 || b == '/' || b == '\\') return {};
        w.push_back(static_cast<wchar_t>(b));
    }
    return w;
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

// Atomic create-or-conflict for the gzip extraction case (single output file).
// Mirrors the Extractor's helper but reduced to one destination — no rename
// retry loop needed because the user's choice applies to a single decision.
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

}  // namespace

bool LooksLikeGz(const fs::path& p) {
    std::wstring lc = ToLowerCopy(p.filename().wstring());
    return EndsWith(lc, L".gz") || EndsWith(lc, L".tgz") || EndsWith(lc, L".taz");
}

bool HasGzMagic(const fs::path& p) {
    std::wstring lp = MakeLongPath(p);
    HANDLE h = ::CreateFileW(lp.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t magic[2] = {};
    DWORD n = 0;
    bool ok = ::ReadFile(h, magic, sizeof(magic), &n, nullptr) &&
              n == 2 && magic[0] == 0x1f && magic[1] == 0x8b;
    ::CloseHandle(h);
    return ok;
}

std::vector<Extractor::Entry> ListEntries(const fs::path& gz_path) {
    std::vector<Extractor::Entry> out;
    GzMeta meta = ReadGzMeta(gz_path);
    if (!meta.ok) return out;

    std::wstring inner_name;
    if (!meta.fname.empty()) {
        inner_name = DecodeAndValidateFname(meta.fname);
    }
    if (inner_name.empty()) {
        std::wstring fn = gz_path.filename().wstring();
        inner_name = StripGzSuffix(ToLowerCopy(fn), fn);
    }
    if (inner_name.empty()) inner_name = L"file";

    Extractor::Entry e;
    e.name = inner_name;
    e.decode_source = DecodeSource::Utf8;  // FNAME is plain bytes; treat as
                                           // UTF-8 for display tagging only.
    // ISIZE is mod 2^32 — a non-zero value is a lower bound and useful for
    // progress, but for files >= 4 GiB it's a wrapped count. We expose it as
    // best-effort and let the bomb guard treat it as approximate.
    e.uncompressed_size = meta.isize_mod32;
    e.compressed_size   = FileSize(gz_path);
    e.modified_time     = 0;  // not tracked; gzip MTIME field omitted on purpose
    e.created_time      = 0;
    e.is_dir            = false;
    e.is_symlink        = false;
    e.needs_password    = false;  // gzip has no encryption layer
    out.push_back(std::move(e));
    return out;
}

Extractor::Result Extract(const fs::path& gz_path,
                          const fs::path& target_dir,
                          Extractor::ProgressCallback& cb) {
    using Result = Extractor::Result;
    auto finish = [&](Result r) -> Result { cb.OnComplete(r); return r; };

    if (!fs::exists(gz_path)) return finish(Result::IoError);
    {
        std::error_code ec;
        fs::create_directories(target_dir, ec);
        if (ec && !fs::exists(target_dir)) return finish(Result::IoError);
    }

    auto entries = ListEntries(gz_path);
    if (entries.empty()) return finish(Result::CorruptArchive);
    Extractor::Entry& e = entries[0];

    // Validate the inner filename against target_dir — same machinery as zip
    // path validation so a hostile FNAME ("../../etc/passwd", ":alt", etc.)
    // is rejected before we open any file.
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

    // Best-effort progress total. ISIZE wraps at 4 GiB; we use compressed size
    // as a fallback so the progress bar still moves on huge archives.
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

    gzFile gz = ::gzopen_w(gz_path.wstring().c_str(), "rb");
    if (!gz) {
        ::CloseHandle(out_h);
        ::DeleteFileW(dest_w.c_str());
        return finish(Result::CorruptArchive);
    }
    ::gzbuffer(gz, kReadBufSize);

    std::array<uint8_t, kReadBufSize> buf{};
    uint64_t bytes_done = 0;
    Result r = Result::Success;
    while (true) {
        if (cb.ShouldCancel()) { r = Result::Cancelled; break; }
        int got = ::gzread(gz, buf.data(), static_cast<unsigned>(buf.size()));
        if (got < 0) { r = Result::CorruptArchive; break; }
        if (got == 0) {
            // EOF for clean reads, but gzread returns 0 also on Z_BUF_ERROR
            // (truncated stream). Disambiguate via gzeof.
            if (!::gzeof(gz)) r = Result::CorruptArchive;
            break;
        }
        DWORD written = 0;
        if (!::WriteFile(out_h, buf.data(), static_cast<DWORD>(got), &written, nullptr) ||
            written != static_cast<DWORD>(got)) {
            r = Result::IoError;
            break;
        }
        bytes_done += static_cast<uint64_t>(got);
        cb.OnBytes(bytes_done, std::max(total, bytes_done));

        // Bomb guard mirrors the zip path: ratio-bad AND size-bad.
        if (e.compressed_size > 0) {
            uint64_t ratio = bytes_done / e.compressed_size;
            if (ratio >= Extractor::kBombRatioLimit &&
                bytes_done >= Extractor::kBombPerEntryLimit) {
                r = Result::BombRefused;
                break;
            }
        }
    }

    ::gzclose(gz);
    ::CloseHandle(out_h);
    if (r != Result::Success) {
        ::DeleteFileW(dest_w.c_str());
    }
    return finish(r);
}

Compressor::Result Compress(const fs::path& source,
                            const fs::path& output_gz,
                            Compressor::ProgressCallback& cb,
                            const Compressor::Options& opts) {
    using Result = Compressor::Result;

    if (!fs::exists(source)) {
        cb.OnComplete(Result::SourceMissing);
        return Result::SourceMissing;
    }
    // Folders aren't representable as a single gzip stream — caller must pick
    // a multi-file format (zip / tar.gz). We surface this as IoError to match
    // the existing "cannot produce output" code path; the GUI/CLI layers
    // catch the case before reaching here.
    if (!fs::is_regular_file(source)) {
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }

    fs::path effective_output = output_gz;
    if (fs::exists(output_gz)) {
        auto act = cb.OnOutputExists(output_gz);
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
                    fs::path candidate = output_gz;
                    candidate.replace_filename(
                        output_gz.stem().wstring() + suffix + output_gz.extension().wstring());
                    if (!fs::exists(candidate)) { effective_output = candidate; break; }
                }
                break;
        }
    }

    fs::path partial = effective_output;
    partial += L".partial";

    // Resolve compression level. Gzip uses zlib levels 1-9 directly; Store
    // (level 0) means "no compression" which gzip supports as well (deflate
    // with stored blocks). zlib's gzopen mode string carries the level.
    int level_int = static_cast<int>(opts.level);
    if (level_int < 0) level_int = 0;
    if (level_int > 9) level_int = 9;
    char mode[8];
    ::sprintf_s(mode, "wb%d", level_int);

    gzFile gz = ::gzopen_w(partial.wstring().c_str(), mode);
    if (!gz) {
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }
    ::gzbuffer(gz, kReadBufSize);

    // Note: zlib's high-level gzopen API doesn't expose the FNAME header.
    // Produced .gz files therefore omit the original filename; extraction
    // recovers it by stripping the `.gz` suffix from the output path. This
    // matches the behaviour of `gzip` invoked without `--name`.

    HANDLE in_h = ::CreateFileW(MakeLongPath(source).c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (in_h == INVALID_HANDLE_VALUE) {
        ::gzclose(gz);
        std::error_code ec; fs::remove(partial, ec);
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }

    uint64_t total_bytes = FileSize(source);
    uint64_t bytes_done = 0;
    cb.OnEntryStart(source.filename().wstring(), 0, 1);
    cb.OnBytes(0, total_bytes);

    Result r = Result::Success;
    std::array<uint8_t, kReadBufSize> buf{};
    while (true) {
        if (cb.ShouldCancel()) { r = Result::Cancelled; break; }
        DWORD nread = 0;
        if (!::ReadFile(in_h, buf.data(), static_cast<DWORD>(buf.size()), &nread, nullptr)) {
            r = Result::IoError;
            break;
        }
        if (nread == 0) break;
        int wrote = ::gzwrite(gz, buf.data(), nread);
        if (wrote <= 0 || static_cast<DWORD>(wrote) != nread) {
            r = Result::IoError;
            break;
        }
        bytes_done += nread;
        cb.OnBytes(bytes_done, total_bytes);
    }

    ::CloseHandle(in_h);
    int gz_close_err = ::gzclose(gz);
    if (r == Result::Success && gz_close_err != Z_OK) r = Result::IoError;

    if (r != Result::Success) {
        std::error_code ec; fs::remove(partial, ec);
        cb.OnComplete(r);
        return r;
    }

    std::error_code ec;
    fs::rename(partial, effective_output, ec);
    if (ec) {
        // Overwrite path: drop the existing target and retry.
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

}  // namespace openzip::gzip
