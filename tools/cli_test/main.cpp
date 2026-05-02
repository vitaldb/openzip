// Minimal console harness for OpenZipCore — Phase 1 acceptance test.
//
// Usage:
//   CliTest.exe <zip-path> [target-dir]
//
// Environment variables:
//   OPENZIP_PASSWORD : password to pre-supply for encrypted archives
//   OPENZIP_LIST     : if set (non-empty), only list entries without extracting

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/extractor.h"
#include "core/filename_decoder.h"

namespace fs = std::filesystem;

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

void PutLine(const std::wstring& s) {
    std::string u = ToUtf8(s);
    std::fwrite(u.data(), 1, u.size(), stdout);
    std::fputc('\n', stdout);
}

const char* SourceTag(openzip::DecodeSource s) {
    switch (s) {
        case openzip::DecodeSource::Utf8Flag: return "utf8*";
        case openzip::DecodeSource::Ascii:    return "ascii";
        case openzip::DecodeSource::Utf8:     return "utf8";
        case openzip::DecodeSource::Cp949:    return "cp949";
        case openzip::DecodeSource::Cp437:    return "cp437";
    }
    return "?";
}

class ConsoleCallback : public openzip::Extractor::ProgressCallback {
public:
    explicit ConsoleCallback(std::wstring preset_password)
        : preset_password_(std::move(preset_password)) {}

    void OnEntryStart(const openzip::Extractor::Entry& e, size_t index, size_t total) override {
        std::printf("[%zu/%zu] (%s) ", index + 1, total, SourceTag(e.decode_source));
        PutLine(e.name);
    }

    void OnBytes(uint64_t done, uint64_t total) override {
        if (total == 0) return;
        int pct = static_cast<int>((done * 100) / total);
        if (pct == last_pct_) return;
        last_pct_ = pct;
        std::printf("\r  %3d%%  %llu / %llu bytes",
                    pct, static_cast<unsigned long long>(done),
                    static_cast<unsigned long long>(total));
        std::fflush(stdout);
    }

    std::wstring OnPasswordRequired(const std::wstring& archive,
                                    const std::wstring& entry,
                                    bool was_wrong) override {
        (void)archive;
        (void)entry;
        if (!was_wrong && !preset_password_.empty()) return preset_password_;
        return std::wstring();  // give up — CLI doesn't prompt interactively in v1
    }

    openzip::Extractor::ConflictAction OnFileConflict(const std::wstring& dest) override {
        std::printf("  conflict: ");
        PutLine(dest);
        std::printf("  → overwriting\n");
        return openzip::Extractor::ConflictAction::Overwrite;
    }

    bool ShouldCancel() override { return false; }

    void OnComplete(openzip::Extractor::Result r) override {
        std::printf("\nResult: %s\n", openzip::ResultName(r));
    }

private:
    std::wstring preset_password_;
    int last_pct_ = -1;
};

std::wstring GetEnvW(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD n = ::GetEnvironmentVariableW(name, buf, sizeof(buf) / sizeof(buf[0]));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return {};
    return std::wstring(buf, n);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        std::printf("Usage: CliTest.exe <zip-path> [target-dir]\n");
        return 2;
    }

    fs::path zip_path = argv[1];
    fs::path target_dir;
    if (argc >= 3) {
        target_dir = argv[2];
    } else {
        target_dir = fs::current_path() / zip_path.stem();
    }

    if (!GetEnvW(L"OPENZIP_LIST").empty()) {
        auto entries = openzip::Extractor::ListEntries(zip_path);
        std::printf("entries: %zu\n", entries.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            std::printf("  %4zu  %s%s  %12llu  %s ",
                        i,
                        e.is_dir ? "d" : "-",
                        e.needs_password ? "p" : "-",
                        static_cast<unsigned long long>(e.uncompressed_size),
                        SourceTag(e.decode_source));
            PutLine(e.name);
        }
        return 0;
    }

    std::printf("Source : %s\n", ToUtf8(zip_path.wstring()).c_str());
    std::printf("Target : %s\n", ToUtf8(target_dir.wstring()).c_str());

    ConsoleCallback cb(GetEnvW(L"OPENZIP_PASSWORD"));
    auto r = openzip::Extractor::Extract(zip_path, target_dir, cb);
    return r == openzip::Extractor::Result::Success ? 0 : 1;
}
