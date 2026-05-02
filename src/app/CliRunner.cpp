#include "stdafx.h"
#include "CliRunner.h"

#include "core/compressor.h"
#include "core/extractor.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace openzip::cli {

namespace fs = std::filesystem;

// ─── Output helpers (UTF-8 console) ──────────────────────────────────────

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

void Out(const std::wstring& s) {
    std::fputs(WideToUtf8(s).c_str(), stdout);
}

void Outln(const std::wstring& s) {
    Out(s);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void Errln(const std::wstring& s) {
    std::fputs("openzip: ", stderr);
    std::fputs(WideToUtf8(s).c_str(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

const wchar_t* DecodeTag(DecodeSource s) {
    switch (s) {
        case DecodeSource::Utf8Flag: return L"utf8*";
        case DecodeSource::Ascii:    return L"ascii";
        case DecodeSource::Utf8:     return L"utf8";
        case DecodeSource::Cp949:    return L"cp949";
        case DecodeSource::Cp437:    return L"cp437";
    }
    return L"?";
}

std::wstring FormatBytes(uint64_t b) {
    constexpr uint64_t K = 1024, M = K * 1024, G = M * 1024;
    wchar_t buf[32];
    if (b >= G)      ::swprintf_s(buf, L"%.2f GB", (double)b / G);
    else if (b >= M) ::swprintf_s(buf, L"%.2f MB", (double)b / M);
    else if (b >= K) ::swprintf_s(buf, L"%.1f KB", (double)b / K);
    else             ::swprintf_s(buf, L"%llu B",  (unsigned long long)b);
    return buf;
}

// Read a password from the console with input echo disabled.
std::wstring PromptPassword(const wchar_t* label) {
    std::fputs(WideToUtf8(label).c_str(), stderr);
    std::fputs(": ", stderr);
    std::fflush(stderr);

    HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD orig = 0;
    bool got_mode = (::GetConsoleMode(in, &orig) != 0);
    if (got_mode) ::SetConsoleMode(in, orig & ~ENABLE_ECHO_INPUT);

    wchar_t buf[256] = {};
    DWORD read = 0;
    BOOL ok = ::ReadConsoleW(in, buf, 255, &read, nullptr);

    if (got_mode) ::SetConsoleMode(in, orig);
    std::fputc('\n', stderr);

    if (!ok || read == 0) return {};
    while (read > 0 && (buf[read - 1] == L'\r' || buf[read - 1] == L'\n')) --read;
    return std::wstring(buf, read);
}

}  // anonymous namespace

// ─── Console callbacks ───────────────────────────────────────────────────

namespace {

class ExtractCb : public Extractor::ProgressCallback {
public:
    explicit ExtractCb(std::wstring preset_pw) : preset_pw_(std::move(preset_pw)) {}

    void OnEntryStart(const Extractor::Entry& e, size_t idx, size_t total) override {
        wchar_t prefix[64];
        ::swprintf_s(prefix, L"[%zu/%zu] (%s) ", idx + 1, total, DecodeTag(e.decode_source));
        Out(prefix);
        Outln(e.name);
        last_pct_ = -1;
    }

    void OnBytes(uint64_t done, uint64_t total) override {
        if (total == 0) return;
        int pct = static_cast<int>((done * 100) / total);
        if (pct == last_pct_) return;
        last_pct_ = pct;
        wchar_t buf[64];
        ::swprintf_s(buf, L"\r       %3d%%  %llu / %llu",
                     pct, (unsigned long long)done, (unsigned long long)total);
        Out(buf);
        std::fflush(stdout);
    }

    std::wstring OnPasswordRequired(const std::wstring& archive,
                                    const std::wstring& entry,
                                    bool was_wrong) override {
        if (!was_wrong && !preset_pw_.empty()) return preset_pw_;
        if (was_wrong) Errln(L"wrong password");
        wchar_t label[256];
        if (entry.empty())
            ::swprintf_s(label, L"password for %s", archive.c_str());
        else
            ::swprintf_s(label, L"password for %s (entry %s)",
                         archive.c_str(), entry.c_str());
        return PromptPassword(label);
    }

    Extractor::ConflictAction OnFileConflict(const std::wstring& dest) override {
        Errln(L"overwriting existing: " + dest);
        return Extractor::ConflictAction::Overwrite;
    }

    bool ShouldCancel() override { return false; }

    void OnComplete(Extractor::Result r) override {
        std::fputc('\n', stdout);
        if (r != Extractor::Result::Success) {
            Errln(std::wstring(L"failed: ") + WideFromUtf8(ResultName(r)));
        }
    }

private:
    static std::wstring WideFromUtf8(const char* s) {
        if (!s) return {};
        int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (n <= 0) return {};
        std::wstring out(static_cast<size_t>(n - 1), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
        return out;
    }
    std::wstring preset_pw_;
    int last_pct_ = -1;
};

class CompressCb : public Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring& rel, size_t idx, size_t total) override {
        wchar_t prefix[64];
        ::swprintf_s(prefix, L"[%zu/%zu] ", idx + 1, total);
        Out(prefix);
        Outln(rel);
        last_pct_ = -1;
    }

    void OnBytes(uint64_t done, uint64_t total) override {
        if (total == 0) return;
        int pct = static_cast<int>((done * 100) / total);
        if (pct == last_pct_) return;
        last_pct_ = pct;
        wchar_t buf[64];
        ::swprintf_s(buf, L"\r       %3d%%  %llu / %llu",
                     pct, (unsigned long long)done, (unsigned long long)total);
        Out(buf);
        std::fflush(stdout);
    }

    Extractor::ConflictAction OnOutputExists(const fs::path& zip) override {
        Errln(L"overwriting existing output: " + zip.wstring());
        return Extractor::ConflictAction::Overwrite;
    }

    bool ShouldCancel() override { return false; }

    void OnComplete(Compressor::Result r) override {
        std::fputc('\n', stdout);
        if (r != Compressor::Result::Success) {
            const char* name = CompressResultName(r);
            std::fputs("openzip: failed: ", stderr);
            std::fputs(name ? name : "Unknown", stderr);
            std::fputc('\n', stderr);
        }
    }

private:
    int last_pct_ = -1;
};

}  // anonymous namespace

// ─── Subcommand impls ────────────────────────────────────────────────────

namespace {

int RunList(const CommandLine& cl) {
    auto entries = Extractor::ListEntries(cl.zip_path);
    if (entries.empty()) {
        Errln(L"could not open or empty: " + cl.zip_path.wstring());
        return 2;
    }
    Outln(L"Archive: " + cl.zip_path.wstring());
    Outln(L"  entries: " + std::to_wstring(entries.size()));
    Outln(L"");
    Outln(L"  IDX  TYPE  ENC    SIZE              NAME");

    uint64_t total_uncompressed = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (!e.is_dir) total_uncompressed += e.uncompressed_size;
        wchar_t row[256];
        ::swprintf_s(row, L"  %4zu  %s%s    %-5s  %14llu  ",
                     i,
                     e.is_dir ? L"d" : L"-",
                     e.needs_password ? L"p" : L"-",
                     DecodeTag(e.decode_source),
                     (unsigned long long)e.uncompressed_size);
        Out(row);
        Outln(e.name);
    }
    Outln(L"");
    Outln(L"  total: " + FormatBytes(total_uncompressed));
    return 0;
}

int RunExtract(const CommandLine& cl) {
    if (!fs::exists(cl.zip_path)) {
        Errln(L"file not found: " + cl.zip_path.wstring());
        return 2;
    }
    Outln(L"Source: " + cl.zip_path.wstring());
    Outln(L"Target: " + cl.target_dir.wstring());

    ExtractCb cb(cl.password);
    Extractor::Options opts;
    // CLI default: single-threaded so progress lines and entry names don't
    // interleave from multiple workers. Users can override with --threads N.
    opts.concurrency = (cl.threads > 0) ? cl.threads : 1;
    auto r = Extractor::Extract(cl.zip_path, cl.target_dir, cb, opts);
    return r == Extractor::Result::Success ? 0 : 1;
}

int RunCompress(const CommandLine& cl) {
    if (cl.compress_items.empty()) {
        Errln(L"--compress requires at least one --item");
        return 2;
    }

    // Resolve output path. Bundle: must have --output. Each: per-item. Prompt:
    // also requires --output in CLI mode (the GUI dialog is unavailable).
    Compressor::Options opts;
    opts.level = static_cast<Compressor::Level>(cl.compress_level);
    opts.password = cl.password;
    if (cl.compress_encoding == L"cp949") opts.filename_encoding = Compressor::Encoding::Cp949;
    else                                  opts.filename_encoding = Compressor::Encoding::Utf8;

    if (cl.compress_mode == CompressMode::Each) {
        int rc = 0;
        for (size_t i = 0; i < cl.compress_items.size(); ++i) {
            const auto& src = cl.compress_items[i];
            fs::path out = src.parent_path() / (src.stem().wstring() + L".zip");
            wchar_t hdr[64];
            ::swprintf_s(hdr, L"\n--- [%zu/%zu] ", i + 1, cl.compress_items.size());
            Out(hdr); Outln(out.wstring());
            CompressCb cb;
            auto r = Compressor::Compress({src}, out, cb, opts);
            if (r != Compressor::Result::Success) rc = 1;
        }
        return rc;
    }

    // Bundle / Prompt → single output (Prompt falls back to Bundle in CLI).
    if (cl.compress_output.empty()) {
        Errln(L"--compress (CLI) requires --output");
        return 2;
    }
    Outln(L"Output: " + cl.compress_output.wstring());
    CompressCb cb;
    auto r = Compressor::Compress(cl.compress_items, cl.compress_output, cb, opts);
    return r == Compressor::Result::Success ? 0 : 1;
}

}  // anonymous namespace

int Run(const CommandLine& cl) {
    if (cl.kind == JobKind::Compress) return RunCompress(cl);
    if (cl.show_browser)              return RunList(cl);  // bare zip path → list
    return RunExtract(cl);
}

}  // namespace openzip::cli
