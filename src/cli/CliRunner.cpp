#include "CliRunner.h"

#include "core/compressor.h"
#include "core/extractor.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
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

// Buffered ordered emitter — accepts (index, line) pairs from any worker
// thread and flushes them to stdout in strict index order. Lets parallel
// extraction proceed at full speed while presenting clean sequential output.
class OrderedEmitter {
public:
    void Emit(size_t idx, std::wstring line) {
        std::lock_guard<std::mutex> lk(m_);
        pending_.emplace(idx, std::move(line));
        Drain();
    }

    void FlushAll() {  // call from OnComplete
        std::lock_guard<std::mutex> lk(m_);
        for (auto& [k, v] : pending_) Write(v);
        pending_.clear();
    }

private:
    void Drain() {
        for (;;) {
            auto it = pending_.find(next_);
            if (it == pending_.end()) return;
            Write(it->second);
            pending_.erase(it);
            ++next_;
        }
    }
    static void Write(const std::wstring& w) {
        if (w.empty()) { std::fputc('\n', stdout); return; }
        std::fputs(WideToUtf8(w).c_str(), stdout);
        std::fputc('\n', stdout);
    }

    std::mutex m_;
    std::map<size_t, std::wstring> pending_;
    size_t next_ = 0;
};

class ExtractCb : public Extractor::ProgressCallback {
public:
    explicit ExtractCb(std::wstring preset_pw) : preset_pw_(std::move(preset_pw)) {}

    void OnEntryStart(const Extractor::Entry& e, size_t idx, size_t total) override {
        // Format the line up front; the emitter handles ordering across threads.
        wchar_t prefix[80];
        ::swprintf_s(prefix, L"[%*zu/%zu] (%-5s) ",
                     IndexWidth(total), idx + 1, total, DecodeTag(e.decode_source));
        std::wstring line = std::wstring(prefix) + e.name;
        if (!e.is_dir) line += L"  (" + FormatBytes(e.uncompressed_size) + L")";
        emitter_.Emit(idx, std::move(line));
    }

    // Bytes are global cumulative — we ignore per-byte updates in CLI mode.
    // The per-entry lines (printed in order via OrderedEmitter) carry enough
    // signal; a chattering percentage line would just be noise in pipes.
    void OnBytes(uint64_t, uint64_t) override {}

    std::wstring OnPasswordRequired(const std::wstring& archive,
                                    const std::wstring& entry,
                                    bool was_wrong) override {
        std::lock_guard<std::mutex> lk(prompt_mtx_);
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
        // Default: overwrite (CLI is non-interactive). Warn on stderr so the
        // user's log shows what was clobbered without polluting stdout.
        Errln(L"overwriting: " + dest);
        return Extractor::ConflictAction::Overwrite;
    }

    bool ShouldCancel() override { return false; }

    void OnComplete(Extractor::Result r) override {
        emitter_.FlushAll();
        if (r != Extractor::Result::Success) {
            const char* name = ResultName(r);
            std::fputs("openzip: failed: ", stderr);
            std::fputs(name ? name : "Unknown", stderr);
            std::fputc('\n', stderr);
        }
    }

private:
    static int IndexWidth(size_t total) {
        int w = 1;
        while (total >= 10) { ++w; total /= 10; }
        return w;
    }

    OrderedEmitter emitter_;
    std::mutex prompt_mtx_;
    std::wstring preset_pw_;
};

class CompressCb : public Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring& rel, size_t idx, size_t total) override {
        wchar_t prefix[64];
        ::swprintf_s(prefix, L"[%*zu/%zu] ", IndexWidth(total), idx + 1, total);
        emitter_.Emit(idx, std::wstring(prefix) + rel);
    }
    void OnBytes(uint64_t, uint64_t) override {}
    Extractor::ConflictAction OnOutputExists(const fs::path& zip) override {
        Errln(L"overwriting output: " + zip.wstring());
        return Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(Compressor::Result r) override {
        emitter_.FlushAll();
        if (r != Compressor::Result::Success) {
            const char* name = CompressResultName(r);
            std::fputs("openzip: failed: ", stderr);
            std::fputs(name ? name : "Unknown", stderr);
            std::fputc('\n', stderr);
        }
    }
private:
    static int IndexWidth(size_t total) {
        int w = 1;
        while (total >= 10) { ++w; total /= 10; }
        return w;
    }
    OrderedEmitter emitter_;
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

    // Pre-stat for the closing summary; cheap (single-pass over central directory).
    auto entries = Extractor::ListEntries(cl.zip_path);
    uint64_t total_bytes = 0;
    size_t   total_files = 0;
    for (const auto& e : entries) {
        if (!e.is_dir) { ++total_files; total_bytes += e.uncompressed_size; }
    }

    ExtractCb cb(cl.password);
    Extractor::Options opts;
    // Use full parallelism — CliRunner's OrderedEmitter buffers per-entry
    // lines so output stays in zip-index order even with concurrent workers.
    opts.concurrency = cl.threads;
    auto r = Extractor::Extract(cl.zip_path, cl.target_dir, cb, opts);

    if (r == Extractor::Result::Success) {
        wchar_t summary[256];
        ::swprintf_s(summary, L"Extracted %zu file%s (%s) to %s",
                     total_files, total_files == 1 ? L"" : L"s",
                     FormatBytes(total_bytes).c_str(),
                     cl.target_dir.wstring().c_str());
        Outln(summary);
    }
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
            // Folders keep the whole filename — `stem()` would chop off
            // anything past the last dot (e.g. "inspire 1.4.2" → "inspire 1.4").
            std::wstring base = fs::is_directory(src)
                ? src.filename().wstring()
                : src.stem().wstring();
            fs::path out = src.parent_path() / (base + L".zip");
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
