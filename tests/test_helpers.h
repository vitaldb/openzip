#pragma once

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace openzip::test {

// Self-deleting RAII temp directory under %TEMP%\OpenZipTests\<random>.
class TempDir {
public:
    TempDir() {
        wchar_t base[MAX_PATH];
        ::GetTempPathW(MAX_PATH, base);
        for (int attempt = 0; attempt < 8; ++attempt) {
            UUID u; ::UuidCreate(&u);
            wchar_t name[64];
            ::swprintf_s(name, L"OpenZipTests_%08lx%04x", u.Data1, u.Data2);
            path_ = fs::path(base) / name;
            std::error_code ec;
            if (fs::create_directories(path_, ec)) return;
        }
        throw std::runtime_error("TempDir: could not create unique directory");
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }
    fs::path operator/(const std::wstring& sub) const { return path_ / sub; }

private:
    fs::path path_;
};

// Throw on I/O failure so gtest reports a clear test failure rather than a
// downstream assertion mismatch (or, in ReadFileBytes' case, a bad_alloc when
// tellg() returns -1 for a missing file and the cast wraps to ~16 EiB).
inline void WriteFileBytes(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    if (!o) throw std::runtime_error("WriteFileBytes: cannot open " + p.string());
    o.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!o) throw std::runtime_error("WriteFileBytes: write failed for " + p.string());
}

inline std::vector<uint8_t> ReadFileBytes(const fs::path& p) {
    std::ifstream i(p, std::ios::binary | std::ios::ate);
    if (!i) throw std::runtime_error("ReadFileBytes: cannot open " + p.string());
    auto raw = i.tellg();
    if (raw < 0) throw std::runtime_error("ReadFileBytes: tellg failed for " + p.string());
    auto size = static_cast<size_t>(raw);
    i.seekg(0);
    std::vector<uint8_t> out(size);
    i.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!i) throw std::runtime_error("ReadFileBytes: read failed for " + p.string());
    return out;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          out.data(), len, nullptr, nullptr);
    return out;
}

}  // namespace openzip::test
