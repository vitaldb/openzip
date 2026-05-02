#pragma once

#include <Windows.h>
#include <filesystem>
#include <fstream>
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

// Write `bytes` into `p`, creating parent dirs as needed.
inline void WriteFileBytes(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

inline std::vector<uint8_t> ReadFileBytes(const fs::path& p) {
    std::ifstream i(p, std::ios::binary | std::ios::ate);
    auto size = static_cast<size_t>(i.tellg());
    i.seekg(0);
    std::vector<uint8_t> out(size);
    i.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return out;
}

}  // namespace openzip::test
