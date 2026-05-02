# OpenZip v0.3 — Compression + Windows 10 Context Menu — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `Compressor` engine to OpenZipCore, expose it through new MFC dialogs in OpenZipApp, surface it in both modern (Win11) and classic (Win10) Explorer context menus, and add Win11 dark mode support to all dialogs.

**Architecture:** Symmetric expansion of the existing Extractor pipeline. Single MSIX continues to be the only distribution channel; MinVersion drops to Win10 1903. A new in-process classic shell-extension DLL handles Win10 (and OS-gates itself off on Win11), while the existing modern DLL gains compress verbs.

**Tech Stack:** C++17, MFC (Static), Win32, COM, MSBuild + vcpkg manifest, minizip-ng (compress side already built into the vcpkg port), GoogleTest (newly added via vcpkg), MSIX packaging via existing `msix/build_msix.py`.

**Spec:** `docs/superpowers/specs/2026-05-02-compress-and-win10-menu-design.md`

---

## File Inventory

**Create:**
- `src/core/compressor.h` — public Compressor API
- `src/core/compressor.cpp` — implementation backed by minizip-ng write API
- `src/app/CompressOptionsDialog.h/.cpp` — new MFC dialog
- `src/app/CompressDialog.h/.cpp` — new MFC dialog (compress progress)
- `src/app/dark_theme.h/.cpp` — dark-mode helper, no MFC dependency in header
- `src/shellext_classic/ClassicShellExt.cpp` — IContextMenu + IShellExtInit DLL
- `src/shellext_classic/OpenZipShellExtClassic.def` — DLL exports
- `src/shellext_classic/OpenZipShellExtClassic.vcxproj` — VS project
- `src/shellext_classic/resource.h` + `OpenZipShellExtClassic.rc` — icon resources
- `tests/CoreTests.vcxproj` — GoogleTest harness for OpenZipCore
- `tests/compressor_tests.cpp` — Compressor unit tests
- `tests/test_helpers.h` — shared test fixtures (temp dir, file helpers)

**Modify:**
- `vcpkg.json` — add `gtest`
- `src/core/OpenZipCore.vcxproj` — register new compressor sources
- `src/app/CommandLine.h/.cpp` — extend parser with compress flags
- `src/app/SingleInstance.h/.cpp` — job descriptor union (extract vs compress)
- `src/app/OpenZipApp.cpp` — dispatch on job type
- `src/app/OpenZipApp.rc` + `src/app/resource.h` — new dialog templates and IDs
- `src/app/ExtractDialog.cpp` — wire dark-theme helper
- `src/app/PasswordDialog.cpp` — wire dark-theme helper
- `src/app/ConflictDialog.cpp` — wire dark-theme helper
- `src/app/OpenZipApp.vcxproj` — add new sources
- `src/shellext/ShellExt.cpp` — add compress CmdKinds and CLSIDs
- `src/shellext/OpenZipShellExt.rc` — add compress icon resource (reuse existing)
- `tools/cli_test/main.cpp` — add `--compress` mode for round-trip smoke tests
- `msix/AppxManifest.xml` — MinVersion drop, classic CLSID, classic handler, modern compress verbs, version bump
- `msix/build_msix.py` — copy classic DLL into staging
- `OpenZip.sln` — add `OpenZipShellExtClassic` and `CoreTests` projects
- `README.md` — document compress feature, Win10 support, screenshots

---

## Phase 0 — Test infrastructure (gtest)

The project currently has no unit-test framework — only `tools/cli_test`, a manual harness. Compressor work needs concrete invariants (round-trip equality, atomicity, encoding flags) that are awkward to verify by eye. Adding GoogleTest via vcpkg now pays off across all subsequent phases.

### Task 0.1: Add gtest to vcpkg manifest

**Files:**
- Modify: `vcpkg.json`

- [ ] **Step 1: Edit `vcpkg.json` — add gtest**

```json
{
  "name": "openzip",
  "version-string": "0.1.0",
  "dependencies": [
    { "name": "minizip-ng", "features": ["zlib", "openssl"] },
    "gtest"
  ]
}
```

- [ ] **Step 2: Run vcpkg install**

Run from repo root:
```powershell
vcpkg install --triplet x64-windows-static
```
Expected: `gtest:x64-windows-static` appears in `vcpkg_installed/x64-windows-static/share/gtest/`.

- [ ] **Step 3: Commit**

```powershell
git add vcpkg.json
git commit -m "build: add gtest to vcpkg manifest"
```

### Task 0.2: Create CoreTests project

**Files:**
- Create: `tests/CoreTests.vcxproj`
- Create: `tests/test_helpers.h`
- Create: `tests/compressor_tests.cpp` (placeholder smoke test only — real tests in Phase 1)
- Modify: `OpenZip.sln`

- [ ] **Step 1: Create `tests/CoreTests.vcxproj`**

Model after `tools/cli_test/CliTest.vcxproj` (a console exe linking OpenZipCore). Key bits:

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0"
         xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration><Platform>x64</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration><Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{1A2B3C4D-0005-4000-8000-100000000005}</ProjectGuid>
    <RootNamespace>CoreTests</RootNamespace>
    <ConfigurationType>Application</ConfigurationType>
    <PlatformToolset>v145</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
    <UseDebugLibraries Condition="'$(Configuration)' == 'Debug'">true</UseDebugLibraries>
    <UseDebugLibraries Condition="'$(Configuration)' != 'Debug'">false</UseDebugLibraries>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <Import Project="..\Directory.Build.props" />
  <ItemDefinitionGroup>
    <ClCompile>
      <LanguageStandard>stdcpp17</LanguageStandard>
      <RuntimeLibrary Condition="'$(Configuration)' == 'Debug'">MultiThreadedDebug</RuntimeLibrary>
      <RuntimeLibrary Condition="'$(Configuration)' != 'Debug'">MultiThreaded</RuntimeLibrary>
      <AdditionalIncludeDirectories>$(MSBuildThisFileDirectory)..\src;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
    <Link>
      <SubSystem>Console</SubSystem>
      <AdditionalDependencies>gtest.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="compressor_tests.cpp" />
  </ItemGroup>
  <ItemGroup>
    <ClInclude Include="test_helpers.h" />
  </ItemGroup>
  <ItemGroup>
    <ProjectReference Include="..\src\core\OpenZipCore.vcxproj">
      <Project>{1A2B3C4D-0001-4000-8000-100000000001}</Project>
    </ProjectReference>
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
```

- [ ] **Step 2: Create `tests/test_helpers.h`**

```cpp
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
```

- [ ] **Step 3: Create `tests/compressor_tests.cpp` placeholder**

```cpp
#include "test_helpers.h"
#include <gtest/gtest.h>

TEST(SmokeTest, TempDirRoundtrip) {
    openzip::test::TempDir td;
    auto p = td.path() / L"hello.txt";
    openzip::test::WriteFileBytes(p, {'h', 'i'});
    auto got = openzip::test::ReadFileBytes(p);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], 'h');
    EXPECT_EQ(got[1], 'i');
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

- [ ] **Step 4: Add CoreTests project to `OpenZip.sln`**

After the existing `OpenZipShellExt` project line, add:
```
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "CoreTests", "tests\CoreTests.vcxproj", "{1A2B3C4D-0005-4000-8000-100000000005}"
EndProject
```
And in the `ProjectConfigurationPlatforms` section:
```
{1A2B3C4D-0005-4000-8000-100000000005}.Debug|x64.ActiveCfg = Debug|x64
{1A2B3C4D-0005-4000-8000-100000000005}.Debug|x64.Build.0 = Debug|x64
{1A2B3C4D-0005-4000-8000-100000000005}.Release|x64.ActiveCfg = Release|x64
{1A2B3C4D-0005-4000-8000-100000000005}.Release|x64.Build.0 = Release|x64
```

- [ ] **Step 5: Build and run the smoke test**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:CoreTests
.\x64\Debug\CoreTests.exe
```
Expected: `[==========] 1 test from 1 test suite ran. … [  PASSED  ] 1 test.` Exit code 0.

- [ ] **Step 6: Commit**

```powershell
git add tests/ OpenZip.sln
git commit -m "test: add gtest harness with smoke test"
```

---

## Phase 1 — Compressor engine (TDD)

Each task in this phase: write failing test → run to confirm fail → implement minimal code → run to confirm pass → commit. The signature/snapshot of `compressor.h` grows incrementally.

### Task 1.1: Compressor header skeleton + first failing test

**Files:**
- Create: `src/core/compressor.h`
- Modify: `tests/compressor_tests.cpp`
- Modify: `src/core/OpenZipCore.vcxproj` — register the new header

- [ ] **Step 1: Write `src/core/compressor.h`**

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "extractor.h"  // for ConflictAction

namespace openzip {

class Compressor {
public:
    enum class Result {
        Success,
        Cancelled,
        IoError,
        SourceMissing,
        OutputExists,
        BadPassword,  // reserved
    };

    enum class Level { Store = 0, Fast = 1, Normal = 6, Max = 9 };
    enum class Encoding { Utf8, Cp949 };

    struct Options {
        Level level = Level::Normal;
        std::wstring password;                  // empty = no encryption
        Encoding filename_encoding = Encoding::Utf8;
        int concurrency = 0;                    // reserved for v1.x
    };

    class ProgressCallback {
    public:
        virtual ~ProgressCallback() = default;
        virtual void OnEntryStart(const std::wstring& src_relpath,
                                  size_t index, size_t total_entries) = 0;
        virtual void OnBytes(uint64_t done, uint64_t total) = 0;
        virtual Extractor::ConflictAction OnOutputExists(
            const std::filesystem::path& output_zip) = 0;
        virtual bool ShouldCancel() = 0;
        virtual void OnComplete(Result) = 0;
    };

    static Result Compress(const std::vector<std::filesystem::path>& sources,
                           const std::filesystem::path& output_zip,
                           ProgressCallback& cb,
                           const Options& opts = {});
};

const char* CompressResultName(Compressor::Result r);

}  // namespace openzip
```

- [ ] **Step 2: Add a failing test for "compress empty source list creates a valid empty zip"**

Append to `tests/compressor_tests.cpp`:
```cpp
#include "core/compressor.h"
#include "core/extractor.h"

namespace { class NullCallback : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override {}
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result) override {}
}; }

TEST(Compressor, EmptySourcesProducesEmptyValidZip) {
    openzip::test::TempDir td;
    fs::path zip = td / L"empty.zip";
    NullCallback cb;
    auto r = openzip::Compressor::Compress({}, zip, cb);
    ASSERT_EQ(r, openzip::Compressor::Result::Success);
    ASSERT_TRUE(fs::exists(zip));
    auto entries = openzip::Extractor::ListEntries(zip);
    EXPECT_EQ(entries.size(), 0u);
}
```

- [ ] **Step 3: Register `compressor.h` in `OpenZipCore.vcxproj`**

Add to the existing `<ItemGroup>` of `<ClInclude>`:
```xml
<ClInclude Include="compressor.h" />
```

- [ ] **Step 4: Run tests to confirm link failure**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:CoreTests
```
Expected: link error `unresolved external symbol "openzip::Compressor::Compress"`.

- [ ] **Step 5: Create `src/core/compressor.cpp` with a minimal implementation that opens an output zip via minizip-ng and immediately closes it**

```cpp
#include "compressor.h"

#include <Windows.h>
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

// Convert a wide path to the multi-byte codepage minizip-ng expects (utf-8 by default).
std::string PathToMz(const fs::path& p) { return WideToCodepage(p.wstring(), CP_UTF8); }

}  // namespace

Compressor::Result Compressor::Compress(const std::vector<fs::path>& sources,
                                        const fs::path& output_zip,
                                        ProgressCallback& cb,
                                        const Options& /*opts*/) {
    void* writer = mz_zip_writer_create();
    if (!writer) {
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }

    fs::path partial = output_zip;
    partial += L".partial";

    int32_t err = mz_zip_writer_open_file(writer, PathToMz(partial).c_str(), 0, 0);
    if (err != MZ_OK) {
        mz_zip_writer_delete(&writer);
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }

    // (file iteration in later tasks)
    (void)sources;

    err = mz_zip_writer_close(writer);
    mz_zip_writer_delete(&writer);
    if (err != MZ_OK) {
        std::error_code ec; fs::remove(partial, ec);
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }

    std::error_code ec;
    fs::rename(partial, output_zip, ec);
    if (ec) {
        fs::remove(partial, ec);
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }
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
```

- [ ] **Step 6: Register `compressor.cpp` in `OpenZipCore.vcxproj`**

Add:
```xml
<ClCompile Include="compressor.cpp" />
```

- [ ] **Step 7: Build and run tests**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:CoreTests
.\x64\Debug\CoreTests.exe
```
Expected: 2 tests pass (smoke + EmptySourcesProducesEmptyValidZip).

- [ ] **Step 8: Commit**

```powershell
git add src/core/compressor.h src/core/compressor.cpp src/core/OpenZipCore.vcxproj tests/compressor_tests.cpp
git commit -m "feat(core): Compressor produces valid empty zip"
```

### Task 1.2: Compress a single file

**Files:**
- Modify: `src/core/compressor.cpp`
- Modify: `tests/compressor_tests.cpp`

- [ ] **Step 1: Add the failing test**

```cpp
TEST(Compressor, SingleFileRoundtrip) {
    openzip::test::TempDir td;
    fs::path src_dir = td / L"src";
    fs::create_directories(src_dir);
    fs::path src_file = src_dir / L"hello.txt";
    openzip::test::WriteFileBytes(src_file, {'h','e','l','l','o'});

    fs::path zip = td / L"out.zip";
    NullCallback cb;
    auto r = openzip::Compressor::Compress({src_file}, zip, cb);
    ASSERT_EQ(r, openzip::Compressor::Result::Success);

    auto entries = openzip::Extractor::ListEntries(zip);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, L"hello.txt");
    EXPECT_EQ(entries[0].uncompressed_size, 5u);
}
```

- [ ] **Step 2: Run to confirm failure**

```powershell
.\x64\Debug\CoreTests.exe --gtest_filter=Compressor.SingleFileRoundtrip
```
Expected: FAIL `entries.size() == 1u (was 0u)`.

- [ ] **Step 3: Implement single-file iteration in `Compressor::Compress`**

Insert before `mz_zip_writer_close(writer)` in `compressor.cpp`:
```cpp
size_t total = sources.size();
for (size_t i = 0; i < sources.size(); ++i) {
    const fs::path& src = sources[i];
    if (!fs::exists(src)) {
        mz_zip_writer_close(writer);
        mz_zip_writer_delete(&writer);
        std::error_code ec; fs::remove(partial, ec);
        cb.OnComplete(Result::SourceMissing);
        return Result::SourceMissing;
    }
    if (cb.ShouldCancel()) {
        mz_zip_writer_close(writer);
        mz_zip_writer_delete(&writer);
        std::error_code ec; fs::remove(partial, ec);
        cb.OnComplete(Result::Cancelled);
        return Result::Cancelled;
    }

    std::wstring rel = src.filename().wstring();
    cb.OnEntryStart(rel, i, total);

    mz_zip_file file_info{};
    std::string utf8_name = WideToCodepage(rel, CP_UTF8);
    file_info.filename = utf8_name.c_str();
    file_info.flag = MZ_ZIP_FLAG_UTF8;
    file_info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
    file_info.zip64 = MZ_ZIP64_AUTO;

    int32_t add_err = mz_zip_writer_add_file(writer, PathToMz(src).c_str(),
                                             utf8_name.c_str());
    if (add_err != MZ_OK) {
        mz_zip_writer_close(writer);
        mz_zip_writer_delete(&writer);
        std::error_code ec; fs::remove(partial, ec);
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }
}
```

- [ ] **Step 4: Run tests**

```powershell
.\x64\Debug\CoreTests.exe
```
Expected: 3 tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/core/compressor.cpp tests/compressor_tests.cpp
git commit -m "feat(core): Compressor handles single-file source"
```

### Task 1.3: Compress a folder recursively

**Files:**
- Modify: `src/core/compressor.cpp`
- Modify: `tests/compressor_tests.cpp`

- [ ] **Step 1: Add the failing test**

```cpp
TEST(Compressor, FolderRecursive) {
    openzip::test::TempDir td;
    fs::path root = td / L"MyDocs";
    openzip::test::WriteFileBytes(root / L"a.txt", {'a'});
    openzip::test::WriteFileBytes(root / L"sub" / L"b.txt", {'b'});

    fs::path zip = td / L"out.zip";
    NullCallback cb;
    auto r = openzip::Compressor::Compress({root}, zip, cb);
    ASSERT_EQ(r, openzip::Compressor::Result::Success);

    auto entries = openzip::Extractor::ListEntries(zip);
    std::vector<std::wstring> names;
    for (auto& e : entries) names.push_back(e.name);
    std::sort(names.begin(), names.end());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], L"MyDocs/a.txt");
    EXPECT_EQ(names[1], L"MyDocs/sub/b.txt");
}
```

- [ ] **Step 2: Run to confirm failure**

```powershell
.\x64\Debug\CoreTests.exe --gtest_filter=Compressor.FolderRecursive
```
Expected: FAIL — current code only handles files.

- [ ] **Step 3: Refactor the iteration loop to walk directories**

Replace the per-source block in `Compressor::Compress` with a helper that flattens `sources` into a list of `(absolute_path_on_disk, relpath_in_zip)` pairs first, then writes them. Add at top of file:

```cpp
namespace {

struct FlatEntry {
    fs::path on_disk;
    std::wstring rel_in_zip;   // forward-slash-separated, no leading slash
};

void Flatten(const fs::path& src, std::vector<FlatEntry>& out) {
    if (!fs::exists(src)) return;
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

}  // namespace
```

Then replace the source loop body with:
```cpp
std::vector<FlatEntry> flat;
for (const auto& s : sources) {
    if (!fs::exists(s)) {
        mz_zip_writer_close(writer);
        mz_zip_writer_delete(&writer);
        std::error_code ec; fs::remove(partial, ec);
        cb.OnComplete(Result::SourceMissing);
        return Result::SourceMissing;
    }
    Flatten(s, flat);
}

for (size_t i = 0; i < flat.size(); ++i) {
    const auto& fe = flat[i];
    if (cb.ShouldCancel()) {
        mz_zip_writer_close(writer);
        mz_zip_writer_delete(&writer);
        std::error_code ec; fs::remove(partial, ec);
        cb.OnComplete(Result::Cancelled);
        return Result::Cancelled;
    }
    cb.OnEntryStart(fe.rel_in_zip, i, flat.size());
    std::string utf8_name = WideToCodepage(fe.rel_in_zip, CP_UTF8);
    int32_t add_err = mz_zip_writer_add_file(writer, PathToMz(fe.on_disk).c_str(),
                                             utf8_name.c_str());
    if (add_err != MZ_OK) {
        mz_zip_writer_close(writer);
        mz_zip_writer_delete(&writer);
        std::error_code ec; fs::remove(partial, ec);
        cb.OnComplete(Result::IoError);
        return Result::IoError;
    }
}
```

- [ ] **Step 4: Run tests**

```powershell
.\x64\Debug\CoreTests.exe
```
Expected: 4 tests pass (including SingleFile still working).

- [ ] **Step 5: Commit**

```powershell
git add src/core/compressor.cpp tests/compressor_tests.cpp
git commit -m "feat(core): Compressor recurses into folders, skips symlinks"
```

### Task 1.4: Filename encoding option (UTF-8 vs CP949)

**Files:**
- Modify: `src/core/compressor.cpp`
- Modify: `tests/compressor_tests.cpp`

- [ ] **Step 1: Add the failing test for CP949 encoding**

```cpp
TEST(Compressor, Cp949EncodingSetsRawBytesNoUtf8Flag) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"테스트.txt";
    openzip::test::WriteFileBytes(src, {'x'});

    fs::path zip = td / L"out.zip";
    NullCallback cb;
    openzip::Compressor::Options opts;
    opts.filename_encoding = openzip::Compressor::Encoding::Cp949;
    auto r = openzip::Compressor::Compress({src}, zip, cb, opts);
    ASSERT_EQ(r, openzip::Compressor::Result::Success);

    // Open with raw minizip and inspect the entry's flag byte directly.
    void* reader = mz_zip_reader_create();
    ASSERT_EQ(mz_zip_reader_open_file(reader,
        openzip::test::WideToUtf8(zip.wstring()).c_str()), MZ_OK);
    ASSERT_EQ(mz_zip_reader_goto_first_entry(reader), MZ_OK);
    mz_zip_file* info = nullptr;
    ASSERT_EQ(mz_zip_reader_entry_get_info(reader, &info), MZ_OK);
    EXPECT_EQ(info->flag & MZ_ZIP_FLAG_UTF8, 0);
    // EUC-KR bytes for "테스트.txt" start with 0xC5 0xD7
    ASSERT_NE(info->filename, nullptr);
    EXPECT_EQ(static_cast<unsigned char>(info->filename[0]), 0xC5);
    EXPECT_EQ(static_cast<unsigned char>(info->filename[1]), 0xD7);
    mz_zip_reader_delete(&reader);
}
```

Add `WideToUtf8` to `tests/test_helpers.h`:
```cpp
inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          out.data(), len, nullptr, nullptr);
    return out;
}
```

Add includes to the test file:
```cpp
#include <mz.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
```

- [ ] **Step 2: Run to confirm failure**

```powershell
.\x64\Debug\CoreTests.exe --gtest_filter=Compressor.Cp949EncodingSetsRawBytesNoUtf8Flag
```
Expected: FAIL — flag is currently always UTF-8.

- [ ] **Step 3: Apply encoding option in `Compressor::Compress`**

Inside the per-entry loop, replace the encoding/flag setup:
```cpp
UINT cp = (opts.filename_encoding == Encoding::Cp949) ? 949 : CP_UTF8;
std::string mbcs_name = WideToCodepage(fe.rel_in_zip, cp);

mz_zip_file file_info{};
file_info.filename = mbcs_name.c_str();
file_info.flag = (cp == CP_UTF8) ? MZ_ZIP_FLAG_UTF8 : 0;
file_info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
file_info.zip64 = MZ_ZIP64_AUTO;
file_info.modified_date = std::time(nullptr);

int32_t add_err = mz_zip_writer_entry_open(writer, &file_info);
if (add_err == MZ_OK) {
    HANDLE h = ::CreateFileW(fe.on_disk.wstring().c_str(), GENERIC_READ,
                             FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { add_err = MZ_OPEN_ERROR; }
    else {
        std::vector<uint8_t> buf(64 * 1024);
        DWORD nread = 0;
        while (::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &nread, nullptr)
               && nread > 0) {
            int32_t w = mz_zip_writer_entry_write(writer, buf.data(),
                                                  static_cast<int32_t>(nread));
            if (w < 0) { add_err = w; break; }
        }
        ::CloseHandle(h);
    }
    mz_zip_writer_entry_close(writer);
}
if (add_err != MZ_OK) { /* same cleanup as before */ }
```

Note: the previous `mz_zip_writer_add_file` call set its own filename (always utf-8); we replace it with explicit `entry_open` + manual write so we can control `file_info.flag` and `file_info.filename` bytes.

- [ ] **Step 4: Add the matching UTF-8 positive test**

```cpp
TEST(Compressor, Utf8EncodingDefaultSetsFlag) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"테스트.txt";
    openzip::test::WriteFileBytes(src, {'x'});

    fs::path zip = td / L"out.zip";
    NullCallback cb;
    auto r = openzip::Compressor::Compress({src}, zip, cb);
    ASSERT_EQ(r, openzip::Compressor::Result::Success);

    void* reader = mz_zip_reader_create();
    ASSERT_EQ(mz_zip_reader_open_file(reader,
        openzip::test::WideToUtf8(zip.wstring()).c_str()), MZ_OK);
    ASSERT_EQ(mz_zip_reader_goto_first_entry(reader), MZ_OK);
    mz_zip_file* info = nullptr;
    ASSERT_EQ(mz_zip_reader_entry_get_info(reader, &info), MZ_OK);
    EXPECT_EQ(info->flag & MZ_ZIP_FLAG_UTF8, MZ_ZIP_FLAG_UTF8);
    mz_zip_reader_delete(&reader);
}
```

- [ ] **Step 5: Run all tests**

```powershell
.\x64\Debug\CoreTests.exe
```
Expected: 6 tests pass.

- [ ] **Step 6: Commit**

```powershell
git add src/core/compressor.cpp tests/compressor_tests.cpp tests/test_helpers.h
git commit -m "feat(core): Compressor honors UTF-8/CP949 encoding option"
```

### Task 1.5: Compression level

**Files:**
- Modify: `src/core/compressor.cpp`
- Modify: `tests/compressor_tests.cpp`

- [ ] **Step 1: Add the failing test (Store level produces uncompressed entry)**

```cpp
TEST(Compressor, StoreLevelLeavesEntryUncompressed) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"a.bin";
    std::vector<uint8_t> bytes(8192, 0xAB);  // highly compressible
    openzip::test::WriteFileBytes(src, bytes);

    fs::path zip = td / L"out.zip";
    NullCallback cb;
    openzip::Compressor::Options opts;
    opts.level = openzip::Compressor::Level::Store;
    ASSERT_EQ(openzip::Compressor::Compress({src}, zip, cb, opts),
              openzip::Compressor::Result::Success);

    auto entries = openzip::Extractor::ListEntries(zip);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].uncompressed_size, 8192u);
    EXPECT_EQ(entries[0].compressed_size, 8192u);
}
```

- [ ] **Step 2: Run to confirm failure**

```powershell
.\x64\Debug\CoreTests.exe --gtest_filter=Compressor.StoreLevelLeavesEntryUncompressed
```
Expected: FAIL — `compressed_size` will be much smaller than 8192 with default DEFLATE.

- [ ] **Step 3: Apply level**

In `compressor.cpp`, before the entry loop:
```cpp
mz_zip_writer_set_compress_method(writer, MZ_COMPRESS_METHOD_DEFLATE);
mz_zip_writer_set_compress_level(writer, static_cast<int16_t>(opts.level));
if (opts.level == Level::Store) {
    mz_zip_writer_set_compress_method(writer, MZ_COMPRESS_METHOD_STORE);
}
```

In the per-entry block, set `file_info.compression_method` from the selected method instead of hardcoded DEFLATE:
```cpp
file_info.compression_method = (opts.level == Level::Store)
    ? MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE;
```

- [ ] **Step 4: Run tests**

```powershell
.\x64\Debug\CoreTests.exe
```
Expected: 7 tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/core/compressor.cpp tests/compressor_tests.cpp
git commit -m "feat(core): Compressor honors compression level"
```

### Task 1.6: Password / AES-256 encryption

**Files:**
- Modify: `src/core/compressor.cpp`
- Modify: `tests/compressor_tests.cpp`

- [ ] **Step 1: Add the failing round-trip test (encrypt → decrypt)**

```cpp
namespace { class FixedPasswordCallback : public openzip::Extractor::ProgressCallback {
public:
    explicit FixedPasswordCallback(std::wstring pw) : pw_(std::move(pw)) {}
    void OnEntryStart(const openzip::Extractor::Entry&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override {}
    std::wstring OnPasswordRequired(const std::wstring&, const std::wstring&, bool) override {
        return pw_;
    }
    openzip::Extractor::ConflictAction OnFileConflict(const std::wstring&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Extractor::Result) override {}
private:
    std::wstring pw_;
}; }

TEST(Compressor, AesPasswordRoundtrip) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"secret.txt";
    openzip::test::WriteFileBytes(src, {'t','o','p','s','e','c'});

    fs::path zip = td / L"out.zip";
    NullCallback cb_w;
    openzip::Compressor::Options opts;
    opts.password = L"P@ssw0rd!";
    ASSERT_EQ(openzip::Compressor::Compress({src}, zip, cb_w, opts),
              openzip::Compressor::Result::Success);

    auto entries = openzip::Extractor::ListEntries(zip);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_TRUE(entries[0].needs_password);

    fs::path out_dir = td / L"out";
    FixedPasswordCallback cb_r(L"P@ssw0rd!");
    auto er = openzip::Extractor::Extract(zip, out_dir, cb_r);
    ASSERT_EQ(er, openzip::Extractor::Result::Success);
    auto got = openzip::test::ReadFileBytes(out_dir / L"secret.txt");
    EXPECT_EQ(got.size(), 6u);
    EXPECT_EQ(got[0], 't');
}
```

- [ ] **Step 2: Run to confirm failure**

Expected: FAIL — extracted entry not encrypted, but `needs_password` will be false.

- [ ] **Step 3: Implement encryption**

Before the entry loop in `compressor.cpp`:
```cpp
if (!opts.password.empty()) {
    std::string pw_utf8 = WideToCodepage(opts.password, CP_UTF8);
    mz_zip_writer_set_password(writer, pw_utf8.c_str());
    mz_zip_writer_set_aes(writer, 1);  // AES-256
}
```

In the per-entry block:
```cpp
if (!opts.password.empty()) {
    file_info.aes_version = MZ_AES_VERSION;
    file_info.flag |= MZ_ZIP_FLAG_ENCRYPTED;
}
```

- [ ] **Step 4: Run tests**

```powershell
.\x64\Debug\CoreTests.exe
```
Expected: 8 tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/core/compressor.cpp tests/compressor_tests.cpp
git commit -m "feat(core): Compressor supports AES-256 password encryption"
```

### Task 1.7: Atomic output (`.partial` → rename, cancel cleans up)

**Files:**
- Modify: `tests/compressor_tests.cpp`
- (No `compressor.cpp` change — atomic behavior was already implemented in Task 1.1)

- [ ] **Step 1: Add a cancel-mid-run test**

```cpp
namespace { class CancelAfterFirstCallback : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t i, size_t) override {
        if (i >= 1) cancel_ = true;
    }
    void OnBytes(uint64_t, uint64_t) override {}
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return cancel_; }
    void OnComplete(openzip::Compressor::Result r) override { result_ = r; }
    openzip::Compressor::Result result_ = openzip::Compressor::Result::Success;
    bool cancel_ = false;
}; }

TEST(Compressor, CancelDeletesPartialNoFinalZip) {
    openzip::test::TempDir td;
    openzip::test::WriteFileBytes(td / L"src" / L"a.txt", {'a'});
    openzip::test::WriteFileBytes(td / L"src" / L"b.txt", {'b'});

    fs::path zip = td / L"out.zip";
    CancelAfterFirstCallback cb;
    auto r = openzip::Compressor::Compress(
        {td / L"src" / L"a.txt", td / L"src" / L"b.txt"}, zip, cb);
    EXPECT_EQ(r, openzip::Compressor::Result::Cancelled);
    EXPECT_FALSE(fs::exists(zip));
    fs::path partial = zip; partial += L".partial";
    EXPECT_FALSE(fs::exists(partial));
}
```

- [ ] **Step 2: Run tests**

```powershell
.\x64\Debug\CoreTests.exe
```
Expected: 9 tests pass.

- [ ] **Step 3: Commit**

```powershell
git add tests/compressor_tests.cpp
git commit -m "test(core): verify Compressor cancel cleans up partial file"
```

### Task 1.8: Output-exists conflict callback

**Files:**
- Modify: `src/core/compressor.cpp`
- Modify: `tests/compressor_tests.cpp`

- [ ] **Step 1: Add the failing test — existing output prompts callback, Skip leaves untouched**

```cpp
namespace { class SkipOnConflict : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override {}
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Skip;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result r) override { last_ = r; }
    openzip::Compressor::Result last_ = openzip::Compressor::Result::Success;
}; }

TEST(Compressor, OutputExistsSkipPreservesOriginal) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"a.txt";
    openzip::test::WriteFileBytes(src, {'a'});
    fs::path zip = td / L"out.zip";
    openzip::test::WriteFileBytes(zip, {'O','L','D'});  // pre-existing file

    SkipOnConflict cb;
    auto r = openzip::Compressor::Compress({src}, zip, cb);
    EXPECT_EQ(r, openzip::Compressor::Result::OutputExists);
    auto bytes = openzip::test::ReadFileBytes(zip);
    EXPECT_EQ(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 'O');
}
```

Add another test for `Rename` returning a `(1)` suffix:
```cpp
namespace { class RenameOnConflict : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override {}
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Rename;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result) override {}
}; }

TEST(Compressor, OutputExistsRenameWritesSuffixedFile) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"a.txt";
    openzip::test::WriteFileBytes(src, {'a'});
    fs::path zip = td / L"out.zip";
    openzip::test::WriteFileBytes(zip, {'O','L','D'});

    RenameOnConflict cb;
    auto r = openzip::Compressor::Compress({src}, zip, cb);
    EXPECT_EQ(r, openzip::Compressor::Result::Success);
    EXPECT_TRUE(fs::exists(td / L"out (1).zip"));
    auto orig = openzip::test::ReadFileBytes(zip);
    EXPECT_EQ(orig[0], 'O');
}
```

- [ ] **Step 2: Run to confirm failure**

```powershell
.\x64\Debug\CoreTests.exe --gtest_filter=Compressor.OutputExists*
```
Expected: FAIL — current code overwrites unconditionally.

- [ ] **Step 3: Implement the conflict resolution**

In `compressor.cpp`, very near the top of `Compress`, before opening the writer:
```cpp
fs::path effective_output = output_zip;
if (fs::exists(output_zip)) {
    auto action = cb.OnOutputExists(output_zip);
    switch (action) {
        case Extractor::ConflictAction::Overwrite:
            // proceed; rename at end will overwrite
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
```

Update `partial` to derive from `effective_output`:
```cpp
fs::path partial = effective_output;
partial += L".partial";
```

And the final rename:
```cpp
fs::rename(partial, effective_output, ec);
```

For the `Overwrite` case, allow `fs::rename` to fail when the target exists by removing it first:
```cpp
if (ec) {
    fs::remove(effective_output, ec);
    fs::rename(partial, effective_output, ec);
}
```

- [ ] **Step 4: Run tests**

```powershell
.\x64\Debug\CoreTests.exe
```
Expected: 11 tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/core/compressor.cpp tests/compressor_tests.cpp
git commit -m "feat(core): Compressor handles output-exists via callback"
```

### Task 1.9: Progress byte reporting

**Files:**
- Modify: `src/core/compressor.cpp`
- Modify: `tests/compressor_tests.cpp`

- [ ] **Step 1: Add the failing test**

```cpp
namespace { class RecordingCallback : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t total) override {
        total_entries_ = total;
    }
    void OnBytes(uint64_t done, uint64_t total) override {
        last_done_ = done; last_total_ = total;
    }
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result) override {}
    size_t total_entries_ = 0;
    uint64_t last_done_ = 0, last_total_ = 0;
}; }

TEST(Compressor, ProgressBytesReachTotalAtEnd) {
    openzip::test::TempDir td;
    std::vector<uint8_t> blob(50000, 0x42);
    openzip::test::WriteFileBytes(td / L"src" / L"a.bin", blob);
    openzip::test::WriteFileBytes(td / L"src" / L"b.bin", blob);

    fs::path zip = td / L"out.zip";
    RecordingCallback cb;
    ASSERT_EQ(openzip::Compressor::Compress(
        {td / L"src" / L"a.bin", td / L"src" / L"b.bin"}, zip, cb),
        openzip::Compressor::Result::Success);

    EXPECT_EQ(cb.total_entries_, 2u);
    EXPECT_EQ(cb.last_total_, 100000u);
    EXPECT_EQ(cb.last_done_, 100000u);
}
```

- [ ] **Step 2: Run to confirm failure**

Expected: FAIL — `last_done_ == 0`.

- [ ] **Step 3: Implement byte progress**

Before the entry loop, sum total bytes:
```cpp
uint64_t total_bytes = 0;
for (const auto& fe : flat) {
    std::error_code sec;
    total_bytes += fs::file_size(fe.on_disk, sec);
}
uint64_t bytes_done = 0;
```

Inside the per-entry write loop, after each `mz_zip_writer_entry_write`:
```cpp
bytes_done += static_cast<uint64_t>(nread);
cb.OnBytes(bytes_done, total_bytes);
```

- [ ] **Step 4: Run tests**

```powershell
.\x64\Debug\CoreTests.exe
```
Expected: 12 tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/core/compressor.cpp tests/compressor_tests.cpp
git commit -m "feat(core): Compressor reports byte progress"
```

---

## Phase 2 — CLI compress harness

Extend `cli_test` so manual round-trip checks against real archives are easy. Lighter than the gtest harness but verifies the full pipeline against on-disk data.

### Task 2.1: Add `--compress` mode to `cli_test`

**Files:**
- Modify: `tools/cli_test/main.cpp`
- Modify: `tools/cli_test/CliTest.vcxproj` — link OpenZipCore (already done) is sufficient

- [ ] **Step 1: Extend `main.cpp` with a compress branch**

Before `wmain`, add:
```cpp
class CompressConsoleCb : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring& rel, size_t i, size_t total) override {
        std::printf("[%zu/%zu] ", i + 1, total);
        PutLine(rel);
    }
    void OnBytes(uint64_t done, uint64_t total) override {
        if (total == 0) return;
        int pct = static_cast<int>((done * 100) / total);
        if (pct == last_pct_) return;
        last_pct_ = pct;
        std::printf("\r  %3d%%", pct);
        std::fflush(stdout);
    }
    openzip::Extractor::ConflictAction OnOutputExists(
        const fs::path& zip) override {
        std::printf("output exists: ");
        PutLine(zip.wstring());
        std::printf("  → overwriting\n");
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result r) override {
        std::printf("\nResult: %s\n", openzip::CompressResultName(r));
    }
private:
    int last_pct_ = -1;
};
```

In `wmain`, after the existing usage block, accept `--compress`:
```cpp
if (argc >= 4 && std::wstring(argv[1]) == L"--compress") {
    fs::path output = argv[2];
    std::vector<fs::path> srcs;
    for (int i = 3; i < argc; ++i) srcs.emplace_back(argv[i]);
    CompressConsoleCb cb;
    openzip::Compressor::Options opts;
    if (auto pw = GetEnvW(L"OPENZIP_PASSWORD"); !pw.empty()) opts.password = pw;
    auto r = openzip::Compressor::Compress(srcs, output, cb, opts);
    return r == openzip::Compressor::Result::Success ? 0 : 1;
}
```

- [ ] **Step 2: Build, run round-trip manually**

```powershell
msbuild OpenZip.sln /p:Configuration=Release /p:Platform=x64 /t:CliTest
$tmp = New-TemporaryFile; Remove-Item $tmp; New-Item $tmp.FullName -ItemType Directory | Out-Null
"hello" | Out-File "$($tmp.FullName)\hello.txt" -Encoding utf8 -NoNewline
.\x64\Release\CliTest.exe --compress "$env:TEMP\out.zip" "$($tmp.FullName)\hello.txt"
.\x64\Release\CliTest.exe "$env:TEMP\out.zip" "$env:TEMP\out_extracted"
Get-Content "$env:TEMP\out_extracted\hello.txt"  # should print "hello"
```
Expected: extracted file content matches.

- [ ] **Step 3: Commit**

```powershell
git add tools/cli_test/main.cpp
git commit -m "tools(cli_test): add --compress mode for round-trip checks"
```

---

## Phase 3 — Compress dialogs (MFC) and queue integration

Two new dialogs and command-line plumbing. Manual UI verification per task.

### Task 3.1: Resource templates for both compress dialogs

**Files:**
- Modify: `src/app/OpenZipApp.rc`
- Modify: `src/app/resource.h`

- [ ] **Step 1: Add resource IDs to `resource.h`**

```cpp
#define IDD_COMPRESS_OPTIONS    140
#define IDD_COMPRESS            141

#define IDC_COMPRESS_OUTNAME      2001
#define IDC_COMPRESS_OUTDIR       2002
#define IDC_COMPRESS_BROWSE       2003
#define IDC_COMPRESS_LEVEL_STORE  2004
#define IDC_COMPRESS_LEVEL_FAST   2005
#define IDC_COMPRESS_LEVEL_NORMAL 2006
#define IDC_COMPRESS_LEVEL_MAX    2007
#define IDC_COMPRESS_PASSWORD     2008
#define IDC_COMPRESS_PASSWORD_CFM 2009
#define IDC_COMPRESS_SHOW_PW      2010

#define IDC_COMPRESS_CURRENT      2020
#define IDC_COMPRESS_PROGRESS     2021
#define IDC_COMPRESS_QUEUE        2022
#define IDC_COMPRESS_CANCEL       2023
```

- [ ] **Step 2: Add `IDD_COMPRESS_OPTIONS` template to `OpenZipApp.rc`**

```rc
IDD_COMPRESS_OPTIONS DIALOGEX 0, 0, 320, 220
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU
CAPTION "OpenZip — 압축 옵션"
FONT 9, "Segoe UI", 400, 0, 0x1
BEGIN
    LTEXT           "출력 파일명",       IDC_STATIC, 12, 12, 60, 10
    EDITTEXT        IDC_COMPRESS_OUTNAME, 80, 10, 220, 14, ES_AUTOHSCROLL
    LTEXT           "출력 위치",         IDC_STATIC, 12, 32, 60, 10
    EDITTEXT        IDC_COMPRESS_OUTDIR, 80, 30, 180, 14, ES_AUTOHSCROLL
    PUSHBUTTON      "...",              IDC_COMPRESS_BROWSE, 264, 30, 36, 14
    GROUPBOX        "압축률",           IDC_STATIC, 12, 56, 296, 36
    AUTORADIOBUTTON "저장",   IDC_COMPRESS_LEVEL_STORE,  20, 70, 50, 12, WS_GROUP
    AUTORADIOBUTTON "빠름",   IDC_COMPRESS_LEVEL_FAST,   80, 70, 50, 12
    AUTORADIOBUTTON "보통",   IDC_COMPRESS_LEVEL_NORMAL, 140, 70, 50, 12
    AUTORADIOBUTTON "최대",   IDC_COMPRESS_LEVEL_MAX,    200, 70, 50, 12
    LTEXT           "비밀번호 (선택)",  IDC_STATIC, 12, 104, 80, 10
    EDITTEXT        IDC_COMPRESS_PASSWORD, 80, 102, 220, 14, ES_PASSWORD | ES_AUTOHSCROLL
    LTEXT           "다시 입력",         IDC_STATIC, 12, 124, 60, 10
    EDITTEXT        IDC_COMPRESS_PASSWORD_CFM, 80, 122, 220, 14, ES_PASSWORD | ES_AUTOHSCROLL
    AUTOCHECKBOX    "표시", IDC_COMPRESS_SHOW_PW, 80, 142, 60, 12
    DEFPUSHBUTTON   "확인",   IDOK,     180, 192, 60, 16
    PUSHBUTTON      "취소",   IDCANCEL, 248, 192, 60, 16
END
```

- [ ] **Step 3: Add `IDD_COMPRESS` (progress) template**

```rc
IDD_COMPRESS DIALOGEX 0, 0, 320, 130
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU
CAPTION "OpenZip — 압축"
FONT 9, "Segoe UI", 400, 0, 0x1
BEGIN
    LTEXT           "준비 중...", IDC_COMPRESS_CURRENT, 12, 12, 296, 24, SS_NOPREFIX | SS_ENDELLIPSIS
    CONTROL         "", IDC_COMPRESS_PROGRESS, "msctls_progress32", WS_BORDER, 12, 50, 296, 14
    LTEXT           "", IDC_COMPRESS_QUEUE, 12, 76, 296, 12
    PUSHBUTTON      "취소", IDC_COMPRESS_CANCEL, 250, 100, 60, 16
END
```

- [ ] **Step 4: Build to validate the .rc**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:OpenZipApp
```
Expected: clean build (no rc.exe errors).

- [ ] **Step 5: Commit**

```powershell
git add src/app/OpenZipApp.rc src/app/resource.h
git commit -m "feat(app): add compress dialog resource templates"
```

### Task 3.2: `CCompressOptionsDialog` skeleton

**Files:**
- Create: `src/app/CompressOptionsDialog.h`
- Create: `src/app/CompressOptionsDialog.cpp`
- Modify: `src/app/OpenZipApp.vcxproj`

- [ ] **Step 1: Create `CompressOptionsDialog.h`**

```cpp
#pragma once

#include "stdafx.h"
#include "core/compressor.h"

class CCompressOptionsDialog : public CDialogEx {
public:
    explicit CCompressOptionsDialog(CWnd* pParent = nullptr);
    enum { IDD = IDD_COMPRESS_OPTIONS };

    // Inputs (caller fills before DoModal):
    std::wstring default_output_name;   // "Documents.zip"
    std::wstring default_output_dir;    // parent dir of selection

    // Outputs (after IDOK):
    std::wstring chosen_output_path;     // joined, absolute
    openzip::Compressor::Options chosen_options;

protected:
    BOOL OnInitDialog() override;
    void DoDataExchange(CDataExchange* pDX) override;
    void OnOK() override;
    afx_msg void OnBrowse();
    afx_msg void OnTogglePasswordVisibility();
    DECLARE_MESSAGE_MAP()

private:
    CString outname_;
    CString outdir_;
    CString password_;
    CString password_cfm_;
    int level_ = 2;  // 0=Store,1=Fast,2=Normal,3=Max
    BOOL show_pw_ = FALSE;
};
```

- [ ] **Step 2: Create `CompressOptionsDialog.cpp`**

```cpp
#include "stdafx.h"
#include "CompressOptionsDialog.h"

#include <shlobj_core.h>

CCompressOptionsDialog::CCompressOptionsDialog(CWnd* pParent)
    : CDialogEx(IDD, pParent) {}

BEGIN_MESSAGE_MAP(CCompressOptionsDialog, CDialogEx)
    ON_BN_CLICKED(IDC_COMPRESS_BROWSE, &CCompressOptionsDialog::OnBrowse)
    ON_BN_CLICKED(IDC_COMPRESS_SHOW_PW, &CCompressOptionsDialog::OnTogglePasswordVisibility)
END_MESSAGE_MAP()

void CCompressOptionsDialog::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_COMPRESS_OUTNAME, outname_);
    DDX_Text(pDX, IDC_COMPRESS_OUTDIR, outdir_);
    DDX_Text(pDX, IDC_COMPRESS_PASSWORD, password_);
    DDX_Text(pDX, IDC_COMPRESS_PASSWORD_CFM, password_cfm_);
    DDX_Radio(pDX, IDC_COMPRESS_LEVEL_STORE, level_);
    DDX_Check(pDX, IDC_COMPRESS_SHOW_PW, show_pw_);
}

BOOL CCompressOptionsDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();
    outname_ = default_output_name.c_str();
    outdir_ = default_output_dir.c_str();
    UpdateData(FALSE);
    return TRUE;
}

void CCompressOptionsDialog::OnOK() {
    UpdateData(TRUE);

    if (password_ != password_cfm_) {
        AfxMessageBox(L"비밀번호가 일치하지 않습니다.", MB_ICONWARNING);
        return;
    }
    if (outname_.IsEmpty() || outdir_.IsEmpty()) {
        AfxMessageBox(L"파일명과 경로를 모두 입력하세요.", MB_ICONWARNING);
        return;
    }

    chosen_output_path = (std::filesystem::path(outdir_.GetString())
                         / outname_.GetString()).wstring();
    chosen_options.password = password_.GetString();
    switch (level_) {
        case 0: chosen_options.level = openzip::Compressor::Level::Store; break;
        case 1: chosen_options.level = openzip::Compressor::Level::Fast; break;
        case 2: chosen_options.level = openzip::Compressor::Level::Normal; break;
        case 3: chosen_options.level = openzip::Compressor::Level::Max; break;
    }
    chosen_options.filename_encoding = openzip::Compressor::Encoding::Utf8;

    CDialogEx::OnOK();
}

void CCompressOptionsDialog::OnBrowse() {
    UpdateData(TRUE);
    BROWSEINFOW bi{};
    bi.hwndOwner = GetSafeHwnd();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpszTitle = L"출력 폴더 선택";
    LPITEMIDLIST pidl = ::SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH];
    if (::SHGetPathFromIDListW(pidl, path)) {
        outdir_ = path;
        UpdateData(FALSE);
    }
    ::CoTaskMemFree(pidl);
}

void CCompressOptionsDialog::OnTogglePasswordVisibility() {
    UpdateData(TRUE);
    HWND p1 = GetDlgItem(IDC_COMPRESS_PASSWORD)->GetSafeHwnd();
    HWND p2 = GetDlgItem(IDC_COMPRESS_PASSWORD_CFM)->GetSafeHwnd();
    ::SendMessageW(p1, EM_SETPASSWORDCHAR, show_pw_ ? 0 : L'•', 0);
    ::SendMessageW(p2, EM_SETPASSWORDCHAR, show_pw_ ? 0 : L'•', 0);
    ::InvalidateRect(p1, nullptr, TRUE);
    ::InvalidateRect(p2, nullptr, TRUE);
}
```

- [ ] **Step 3: Register the new sources in `OpenZipApp.vcxproj`**

```xml
<ClCompile Include="CompressOptionsDialog.cpp" />
<ClInclude Include="CompressOptionsDialog.h" />
```

- [ ] **Step 4: Build**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:OpenZipApp
```
Expected: clean build.

- [ ] **Step 5: Commit**

```powershell
git add src/app/CompressOptionsDialog.* src/app/OpenZipApp.vcxproj
git commit -m "feat(app): add CCompressOptionsDialog"
```

### Task 3.3: `CCompressDialog` (progress) skeleton

**Files:**
- Create: `src/app/CompressDialog.h`
- Create: `src/app/CompressDialog.cpp`
- Modify: `src/app/OpenZipApp.vcxproj`

- [ ] **Step 1: Create `CompressDialog.h`**

```cpp
#pragma once

#include "stdafx.h"
#include "core/compressor.h"

#include <atomic>
#include <thread>
#include <vector>

class CCompressDialog : public CDialogEx {
public:
    explicit CCompressDialog(CWnd* pParent = nullptr);
    enum { IDD = IDD_COMPRESS };

    // Caller fills these before DoModal.
    std::vector<std::filesystem::path> sources;
    std::filesystem::path output_path;
    openzip::Compressor::Options options;

    // For multi-archive batches: pos out of total. Pass {0, 0} for single.
    int batch_index = 0;
    int batch_total = 1;

protected:
    BOOL OnInitDialog() override;
    void OnCancel() override;
    afx_msg LRESULT OnEntryStart(WPARAM wp, LPARAM lp);
    afx_msg LRESULT OnBytes(WPARAM wp, LPARAM lp);
    afx_msg LRESULT OnComplete(WPARAM wp, LPARAM lp);
    DECLARE_MESSAGE_MAP()

private:
    static UINT __cdecl WorkerEntry(LPVOID self);
    void RunWorker();

    std::thread worker_;
    std::atomic<bool> cancel_{false};
    openzip::Compressor::Result final_result_ = openzip::Compressor::Result::Success;
};
```

Add custom messages near the top of `CompressDialog.cpp`:
```cpp
constexpr UINT WM_OZ_COMP_ENTRY    = WM_USER + 200;
constexpr UINT WM_OZ_COMP_BYTES    = WM_USER + 201;
constexpr UINT WM_OZ_COMP_COMPLETE = WM_USER + 202;
```

- [ ] **Step 2: Create `CompressDialog.cpp`**

```cpp
#include "stdafx.h"
#include "CompressDialog.h"

#include <new>

constexpr UINT WM_OZ_COMP_ENTRY    = WM_USER + 200;
constexpr UINT WM_OZ_COMP_BYTES    = WM_USER + 201;
constexpr UINT WM_OZ_COMP_COMPLETE = WM_USER + 202;

namespace {

struct EntryMsg { std::wstring rel; size_t i, total; };
struct BytesMsg { uint64_t done, total; };

class DialogCallback : public openzip::Compressor::ProgressCallback {
public:
    explicit DialogCallback(HWND hwnd, std::atomic<bool>* cancel)
        : hwnd_(hwnd), cancel_(cancel) {}

    void OnEntryStart(const std::wstring& rel, size_t i, size_t total) override {
        auto* m = new (std::nothrow) EntryMsg{rel, i, total};
        if (m) ::PostMessageW(hwnd_, WM_OZ_COMP_ENTRY, 0, reinterpret_cast<LPARAM>(m));
    }
    void OnBytes(uint64_t done, uint64_t total) override {
        auto* m = new (std::nothrow) BytesMsg{done, total};
        if (m) ::PostMessageW(hwnd_, WM_OZ_COMP_BYTES, 0, reinterpret_cast<LPARAM>(m));
    }
    openzip::Extractor::ConflictAction OnOutputExists(const std::filesystem::path&) override {
        // For v0.3, batch mode auto-overwrites; interactive mode handled before queue.
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return cancel_->load(); }
    void OnComplete(openzip::Compressor::Result r) override {
        ::PostMessageW(hwnd_, WM_OZ_COMP_COMPLETE,
                       static_cast<WPARAM>(r), 0);
    }
private:
    HWND hwnd_;
    std::atomic<bool>* cancel_;
};

}  // namespace

CCompressDialog::CCompressDialog(CWnd* p) : CDialogEx(IDD, p) {}

BEGIN_MESSAGE_MAP(CCompressDialog, CDialogEx)
    ON_MESSAGE(WM_OZ_COMP_ENTRY,    &CCompressDialog::OnEntryStart)
    ON_MESSAGE(WM_OZ_COMP_BYTES,    &CCompressDialog::OnBytes)
    ON_MESSAGE(WM_OZ_COMP_COMPLETE, &CCompressDialog::OnComplete)
END_MESSAGE_MAP()

BOOL CCompressDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();
    if (auto* pb = (CProgressCtrl*)GetDlgItem(IDC_COMPRESS_PROGRESS))
        pb->SetRange32(0, 1000);
    if (batch_total > 1) {
        CString s;
        s.Format(L"%d / %d", batch_index + 1, batch_total);
        SetDlgItemTextW(IDC_COMPRESS_QUEUE, s);
    }
    worker_ = std::thread([this] { RunWorker(); });
    return TRUE;
}

void CCompressDialog::RunWorker() {
    DialogCallback cb(GetSafeHwnd(), &cancel_);
    final_result_ = openzip::Compressor::Compress(sources, output_path, cb, options);
}

void CCompressDialog::OnCancel() {
    cancel_.store(true);
    SetDlgItemTextW(IDC_COMPRESS_CANCEL, L"취소 중...");
    GetDlgItem(IDC_COMPRESS_CANCEL)->EnableWindow(FALSE);
}

LRESULT CCompressDialog::OnEntryStart(WPARAM, LPARAM lp) {
    std::unique_ptr<EntryMsg> m(reinterpret_cast<EntryMsg*>(lp));
    SetDlgItemTextW(IDC_COMPRESS_CURRENT, m->rel.c_str());
    return 0;
}

LRESULT CCompressDialog::OnBytes(WPARAM, LPARAM lp) {
    std::unique_ptr<BytesMsg> m(reinterpret_cast<BytesMsg*>(lp));
    if (m->total > 0) {
        int v = static_cast<int>((m->done * 1000) / m->total);
        if (auto* pb = (CProgressCtrl*)GetDlgItem(IDC_COMPRESS_PROGRESS))
            pb->SetPos(v);
    }
    return 0;
}

LRESULT CCompressDialog::OnComplete(WPARAM wp, LPARAM) {
    final_result_ = static_cast<openzip::Compressor::Result>(wp);
    if (worker_.joinable()) worker_.join();
    EndDialog(final_result_ == openzip::Compressor::Result::Success ? IDOK : IDABORT);
    return 0;
}
```

- [ ] **Step 3: Register in `OpenZipApp.vcxproj`**

```xml
<ClCompile Include="CompressDialog.cpp" />
<ClInclude Include="CompressDialog.h" />
```

- [ ] **Step 4: Build**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:OpenZipApp
```
Expected: clean build.

- [ ] **Step 5: Commit**

```powershell
git add src/app/CompressDialog.* src/app/OpenZipApp.vcxproj
git commit -m "feat(app): add CCompressDialog progress UI"
```

### Task 3.4: Extend `CommandLine` parser with compress flags

**Files:**
- Modify: `src/app/CommandLine.h`
- Modify: `src/app/CommandLine.cpp`

- [ ] **Step 1: Extend `CommandLine` struct in the header**

```cpp
enum class JobKind { Extract, Compress };
enum class CompressMode { Bundle, Each, Prompt };

struct CommandLine {
    JobKind kind = JobKind::Extract;

    // Extract-mode fields (existing)
    std::filesystem::path zip_path;
    std::filesystem::path target_dir;
    std::wstring password;
    int threads = 0;

    // Compress-mode fields (new)
    std::vector<std::filesystem::path> compress_items;
    std::filesystem::path compress_output;
    CompressMode compress_mode = CompressMode::Bundle;
    int compress_level = 6;             // matches Normal
    std::wstring compress_encoding = L"utf8";

    bool show_help = false;
    bool valid = true;
    std::wstring error;
};
```

Add `#include <vector>` to the header.

- [ ] **Step 2: Extend the parser**

In the for-loop in `ParseCommandLine`, add branches:
```cpp
} else if (a == L"--compress") {
    c.kind = JobKind::Compress;
} else if (a == L"--output") {
    const wchar_t* v = need_value(L"--output"); if (!v) break;
    c.compress_output = v;
} else if (a == L"--mode") {
    const wchar_t* v = need_value(L"--mode"); if (!v) break;
    std::wstring mv = v;
    if      (mv == L"bundle") c.compress_mode = CompressMode::Bundle;
    else if (mv == L"each")   c.compress_mode = CompressMode::Each;
    else if (mv == L"prompt") c.compress_mode = CompressMode::Prompt;
    else { c.valid = false; c.error = L"unknown --mode value: " + mv; break; }
} else if (a == L"--level") {
    const wchar_t* v = need_value(L"--level"); if (!v) break;
    std::wstring lv = v;
    if      (lv == L"store")  c.compress_level = 0;
    else if (lv == L"fast")   c.compress_level = 1;
    else if (lv == L"normal") c.compress_level = 6;
    else if (lv == L"max")    c.compress_level = 9;
    else { c.valid = false; c.error = L"unknown --level value: " + lv; break; }
} else if (a == L"--encoding") {
    const wchar_t* v = need_value(L"--encoding"); if (!v) break;
    c.compress_encoding = v;
} else if (a == L"--item") {
    const wchar_t* v = need_value(L"--item"); if (!v) break;
    c.compress_items.emplace_back(v);
}
```

After the loop, replace the post-validation block with:
```cpp
if (c.show_help || !c.valid) return c;

if (c.kind == JobKind::Compress) {
    if (c.compress_items.empty()) {
        c.valid = false; c.error = L"--compress requires at least one --item"; return c;
    }
    if (c.compress_output.empty() && c.compress_mode != CompressMode::Each) {
        c.valid = false; c.error = L"--compress requires --output (or --mode each)"; return c;
    }
    if (!c.compress_output.empty())
        c.compress_output = fs::absolute(c.compress_output);
    return c;
}

// Extract validation (unchanged)
if (c.zip_path.empty()) { … existing block … }
c.zip_path = fs::absolute(c.zip_path);
c.target_dir = ResolveTarget(c.zip_path, explicit_target, mode);
return c;
```

- [ ] **Step 3: Build**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:OpenZipApp
```
Expected: clean build.

- [ ] **Step 4: Commit**

```powershell
git add src/app/CommandLine.h src/app/CommandLine.cpp
git commit -m "feat(app): parse --compress family of flags"
```

### Task 3.5: Wire compress jobs into the single-instance queue

**Files:**
- Modify: `src/app/OpenZipApp.cpp`

- [ ] **Step 1: Find the existing main message-pump / job-dispatch site**

It currently calls `Extractor::Extract` (or invokes `CExtractDialog::DoModal`) per job pulled from `SingleInstance::PopNext`. Wrap the dispatch in a switch on `JobKind`:

```cpp
while (instance.PopNext(cmdline_str, kQueueWaitMs)) {
    auto cl = openzip::ParseCommandLine(cmdline_str.c_str());
    if (!cl.valid) { /* log + skip */ continue; }
    if (cl.kind == openzip::JobKind::Extract) {
        // existing extract dispatch
        CExtractDialog dlg;
        dlg.zip_path = cl.zip_path;
        dlg.target_dir = cl.target_dir;
        dlg.password = cl.password;
        dlg.DoModal();
    } else {
        if (cl.compress_mode == openzip::CompressMode::Prompt) {
            CCompressOptionsDialog opt;
            opt.default_output_name =
                (cl.compress_items.front().parent_path().filename().wstring() + L".zip");
            opt.default_output_dir = cl.compress_items.front().parent_path().wstring();
            if (opt.DoModal() != IDOK) continue;
            cl.compress_output = opt.chosen_output_path;
            // honor level/password/encoding from dialog
            CCompressDialog cd;
            cd.sources = cl.compress_items;
            cd.output_path = cl.compress_output;
            cd.options = opt.chosen_options;
            cd.DoModal();
        } else if (cl.compress_mode == openzip::CompressMode::Each) {
            for (size_t i = 0; i < cl.compress_items.size(); ++i) {
                const auto& src = cl.compress_items[i];
                std::filesystem::path out = src.parent_path() / (src.stem().wstring() + L".zip");
                CCompressDialog cd;
                cd.sources = {src};
                cd.output_path = out;
                cd.batch_index = static_cast<int>(i);
                cd.batch_total = static_cast<int>(cl.compress_items.size());
                cd.DoModal();
            }
        } else {  // Bundle
            CCompressDialog cd;
            cd.sources = cl.compress_items;
            cd.output_path = cl.compress_output;
            cd.DoModal();
        }
    }
}
```

Add includes at the top of `OpenZipApp.cpp`:
```cpp
#include "CompressOptionsDialog.h"
#include "CompressDialog.h"
```

- [ ] **Step 2: Build**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:OpenZipApp
```
Expected: clean build.

- [ ] **Step 3: Manual smoke — compress via command line**

```powershell
$src = "$env:TEMP\openzip_smoke"; New-Item $src -ItemType Directory -Force | Out-Null
"hi" | Out-File "$src\a.txt" -Encoding utf8 -NoNewline
.\x64\Debug\OpenZipApp.exe --compress --output "$env:TEMP\smoke.zip" --mode bundle --item "$src\a.txt"
Test-Path "$env:TEMP\smoke.zip"  # True expected
```
Expected: progress dialog flashes, exits, file exists.

- [ ] **Step 4: Manual smoke — prompt mode**

```powershell
.\x64\Debug\OpenZipApp.exe --compress --mode prompt --item "$src\a.txt"
```
Expected: options dialog opens with default name "openzip_smoke.zip" pre-filled.

- [ ] **Step 5: Commit**

```powershell
git add src/app/OpenZipApp.cpp
git commit -m "feat(app): dispatch compress jobs through single-instance queue"
```

---

## Phase 4 — Modern shell extension compress verbs

Add new CmdKinds and a separate parent CLSID to `OpenZipShellExt.dll`.

### Task 4.1: New CmdKinds, GUIDs, and titles

**Files:**
- Modify: `src/shellext/ShellExt.cpp`

- [ ] **Step 1: Extend `CmdKind` enum and add new GUIDs**

In the anonymous namespace at the top of `ShellExt.cpp`:
```cpp
constexpr GUID kCLSID_CompressParent = {
    0xA8F3C7E4, 0x1B2D, 0x4F5E,
    {0x9C, 0x8A, 0x3B, 0x6D, 0x2E, 0x5F, 0x1B, 0x00}};
constexpr GUID kCLSID_CompressBundle = {
    0xA8F3C7E4, 0x1B2D, 0x4F5E,
    {0x9C, 0x8A, 0x3B, 0x6D, 0x2E, 0x5F, 0x1B, 0x01}};
constexpr GUID kCLSID_CompressEach = {
    0xA8F3C7E4, 0x1B2D, 0x4F5E,
    {0x9C, 0x8A, 0x3B, 0x6D, 0x2E, 0x5F, 0x1B, 0x02}};
constexpr GUID kCLSID_CompressPrompt = {
    0xA8F3C7E4, 0x1B2D, 0x4F5E,
    {0x9C, 0x8A, 0x3B, 0x6D, 0x2E, 0x5F, 0x1B, 0x03}};

enum class CmdKind {
    Parent, Here, Folder,
    CompressParent, CompressBundle, CompressEach, CompressPrompt,
};
```

- [ ] **Step 2: Add localized title helpers**

```cpp
const wchar_t* TitleCompressParent() { return L"OpenZip"; }
const wchar_t* TitleCompressEach() {
    return IsKoreanLocale() ? L"각각 압축" : L"Compress each separately";
}
const wchar_t* TitleCompressPrompt() {
    return IsKoreanLocale() ? L"압축 옵션…" : L"Compress with options…";
}
std::wstring TitleCompressBundle(IShellItemArray* items) {
    if (!items) return IsKoreanLocale() ? L"압축" : L"Compress";
    DWORD count = 0; items->GetCount(&count);
    std::wstring base_name;
    if (count == 1) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(items->GetItemAt(0, &it)) && it) {
            LPWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                std::wstring full(p);
                ::CoTaskMemFree(p);
                base_name = std::filesystem::path(full).stem().wstring();
            }
            it->Release();
        }
    } else {
        IShellItem* it = nullptr;
        if (SUCCEEDED(items->GetItemAt(0, &it)) && it) {
            LPWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                base_name = std::filesystem::path(p).parent_path().filename().wstring();
                ::CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    if (base_name.empty()) base_name = L"Archive";
    if (IsKoreanLocale()) return L"\"" + base_name + L".zip\"으로 압축";
    return L"Compress to \"" + base_name + L".zip\"";
}
```

Need `#include <filesystem>` near the top.

- [ ] **Step 3: Commit (intermediate, code still compiles)**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:OpenZipShellExt
git add src/shellext/ShellExt.cpp
git commit -m "feat(shellext): declare compress CmdKinds and GUIDs"
```

### Task 4.2: Compress submenu enumerator and command dispatch

**Files:**
- Modify: `src/shellext/ShellExt.cpp`

- [ ] **Step 1: Add a `CompressSubCommandEnum` class (mirror of existing `SubCommandEnum`)**

```cpp
class CompressSubCommandEnum : public IEnumExplorerCommand {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IEnumExplorerCommand) {
            *ppv = static_cast<IEnumExplorerCommand*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = ::InterlockedDecrement(&ref_); if (r == 0) delete this; return r;
    }
    IFACEMETHODIMP Next(ULONG celt, IExplorerCommand** rgelt, ULONG* pceltFetched) override {
        ULONG fetched = 0;
        for (ULONG i = 0; i < celt && cursor_ < kCount; ++i) {
            CmdKind k = (cursor_ == 0) ? CmdKind::CompressBundle
                       : (cursor_ == 1) ? CmdKind::CompressEach
                                        : CmdKind::CompressPrompt;
            auto* cmd = new (std::nothrow) OpenZipCommand(k);
            if (!cmd) return E_OUTOFMEMORY;
            rgelt[i] = cmd; ++cursor_; ++fetched;
        }
        if (pceltFetched) *pceltFetched = fetched;
        return (fetched < celt) ? S_FALSE : S_OK;
    }
    IFACEMETHODIMP Skip(ULONG celt) override {
        cursor_ = (cursor_ + celt > kCount) ? kCount : cursor_ + celt; return S_OK;
    }
    IFACEMETHODIMP Reset() override { cursor_ = 0; return S_OK; }
    IFACEMETHODIMP Clone(IEnumExplorerCommand** out) override {
        auto* c = new (std::nothrow) CompressSubCommandEnum();
        if (!c) return E_OUTOFMEMORY;
        c->cursor_ = cursor_; *out = c; return S_OK;
    }
private:
    static constexpr ULONG kCount = 3;
    LONG ref_ = 1;
    ULONG cursor_ = 0;
};
```

- [ ] **Step 2: Extend `OpenZipCommand` methods to handle compress kinds**

In `GetCanonicalName`:
```cpp
case CmdKind::CompressParent: *guid = kCLSID_CompressParent; break;
case CmdKind::CompressBundle: *guid = kCLSID_CompressBundle; break;
case CmdKind::CompressEach:   *guid = kCLSID_CompressEach;   break;
case CmdKind::CompressPrompt: *guid = kCLSID_CompressPrompt; break;
```

In `GetTitle`:
```cpp
case CmdKind::CompressParent: return CopyToTaskMem(TitleCompressParent(), name);
case CmdKind::CompressBundle: {
    auto t = TitleCompressBundle(items);
    return CopyToTaskMem(t.c_str(), name);
}
case CmdKind::CompressEach:   return CopyToTaskMem(TitleCompressEach(), name);
case CmdKind::CompressPrompt: return CopyToTaskMem(TitleCompressPrompt(), name);
```

In `GetState`:
```cpp
if (kind_ == CmdKind::CompressParent || kind_ == CmdKind::CompressBundle ||
    kind_ == CmdKind::CompressEach || kind_ == CmdKind::CompressPrompt) {
    if (!items) { *state = ECS_HIDDEN; return S_OK; }
    DWORD count = 0;
    if (FAILED(items->GetCount(&count)) || count == 0) {
        *state = ECS_HIDDEN; return S_OK;
    }
    // hide if exactly one .zip selected (extract verb already covers it)
    if (count == 1) {
        auto path = FirstSelectedPath(items);
        if (EndsWithIgnoreCase(path, L".zip")) { *state = ECS_HIDDEN; return S_OK; }
    }
    *state = ECS_ENABLED;
    return S_OK;
}
```
(Place above the existing extract `GetState` body, then preserve the extract logic for `Parent`/`Here`/`Folder`.)

In `GetFlags`:
```cpp
*flags = (kind_ == CmdKind::Parent || kind_ == CmdKind::CompressParent)
    ? ECF_HASSUBCOMMANDS : ECF_DEFAULT;
```

In `EnumSubCommands`:
```cpp
if (kind_ == CmdKind::Parent) {
    auto* e = new (std::nothrow) SubCommandEnum();
    if (!e) return E_OUTOFMEMORY; *out = e; return S_OK;
}
if (kind_ == CmdKind::CompressParent) {
    auto* e = new (std::nothrow) CompressSubCommandEnum();
    if (!e) return E_OUTOFMEMORY; *out = e; return S_OK;
}
return E_NOTIMPL;
```

In `Invoke`, add dispatch for compress verbs (the existing extract block stays):
```cpp
if (kind_ == CmdKind::CompressBundle || kind_ == CmdKind::CompressEach ||
    kind_ == CmdKind::CompressPrompt) {
    DWORD count = 0; items->GetCount(&count);
    std::vector<std::wstring> paths;
    paths.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &it)) && it) {
            LPWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                paths.emplace_back(p); ::CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    if (paths.empty()) return E_INVALIDARG;
    LaunchAppCompress(paths, kind_);
    return S_OK;
}
```

- [ ] **Step 3: Add `LaunchAppCompress` near the existing `LaunchApp`**

```cpp
void LaunchAppCompress(const std::vector<std::wstring>& paths, CmdKind kind) {
    std::wstring exe = AppExePath();
    if (exe.empty() || paths.empty()) return;

    std::wstring cmdline = L"\"" + exe + L"\" --compress";
    if (kind == CmdKind::CompressBundle) {
        std::wstring parent = std::filesystem::path(paths[0]).parent_path().wstring();
        std::wstring base   = (paths.size() == 1)
            ? std::filesystem::path(paths[0]).stem().wstring()
            : std::filesystem::path(parent).filename().wstring();
        cmdline += L" --mode bundle --output \"" + parent + L"\\" + base + L".zip\"";
    } else if (kind == CmdKind::CompressEach) {
        cmdline += L" --mode each";
    } else {
        cmdline += L" --mode prompt";
    }
    for (const auto& p : paths) cmdline += L" --item \"" + p + L"\"";

    wchar_t cwd[MAX_PATH] = L"";
    ::SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, cwd);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (::CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                         0, nullptr, *cwd ? cwd : nullptr, &si, &pi)) {
        ::CloseHandle(pi.hThread); ::CloseHandle(pi.hProcess);
    } else {
        Log(L"LaunchAppCompress CreateProcessW failed err=%lu", ::GetLastError());
    }
}
```

- [ ] **Step 4: Update `DllGetClassObject` to expose the compress parent CLSID**

The existing `ClassFactory::CreateInstance` always constructs a `Parent` (extract) command. Make the factory CLSID-aware: store the kind in the factory, set it from `DllGetClassObject`:

```cpp
class ClassFactory : public IClassFactory {
public:
    explicit ClassFactory(CmdKind kind) : kind_(kind) {}
    // … unchanged QueryInterface/AddRef/Release …
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* cmd = new (std::nothrow) OpenZipCommand(kind_);
        if (!cmd) return E_OUTOFMEMORY;
        HRESULT hr = cmd->QueryInterface(riid, ppv);
        cmd->Release();
        return hr;
    }
    // … LockServer …
private:
    CmdKind kind_;
};
```

Update `DllGetClassObject`:
```cpp
STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    CmdKind kind;
    if      (clsid == kCLSID_OpenZipCommand) kind = CmdKind::Parent;
    else if (clsid == kCLSID_CompressParent) kind = CmdKind::CompressParent;
    else return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) ClassFactory(kind);
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}
```

- [ ] **Step 5: Build**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:OpenZipShellExt
```
Expected: clean build.

- [ ] **Step 6: Commit**

```powershell
git add src/shellext/ShellExt.cpp
git commit -m "feat(shellext): implement modern compress verbs"
```

---

## Phase 5 — Classic shell extension (Win10)

New DLL. Pure Win32, no MFC, in-process to explorer.

### Task 5.1: Create the project skeleton

**Files:**
- Create: `src/shellext_classic/OpenZipShellExtClassic.vcxproj`
- Create: `src/shellext_classic/OpenZipShellExtClassic.def`
- Create: `src/shellext_classic/resource.h`
- Create: `src/shellext_classic/OpenZipShellExtClassic.rc`
- Create: `src/shellext_classic/icon.ico` (copy from `src/shellext/icon.ico`)
- Modify: `OpenZip.sln`

- [ ] **Step 1: Create `OpenZipShellExtClassic.def`**

```
LIBRARY OpenZipShellExtClassic
EXPORTS
    DllGetClassObject       PRIVATE
    DllCanUnloadNow         PRIVATE
```

- [ ] **Step 2: Create `OpenZipShellExtClassic.vcxproj` (model after `src/shellext/OpenZipShellExt.vcxproj`)**

The full file mirrors the existing modern shell ext project but with:
```xml
<ProjectGuid>{1A2B3C4D-0006-4000-8000-100000000006}</ProjectGuid>
<RootNamespace>OpenZipShellExtClassic</RootNamespace>
<ConfigurationType>DynamicLibrary</ConfigurationType>
<UseOfMfc>false</UseOfMfc>
```

And:
```xml
<ItemGroup>
  <ClCompile Include="ClassicShellExt.cpp" />
</ItemGroup>
<ItemGroup>
  <ResourceCompile Include="OpenZipShellExtClassic.rc" />
</ItemGroup>
<ItemGroup>
  <None Include="OpenZipShellExtClassic.def" />
</ItemGroup>
<ItemDefinitionGroup>
  <Link>
    <ModuleDefinitionFile>OpenZipShellExtClassic.def</ModuleDefinitionFile>
    <AdditionalDependencies>shlwapi.lib;%(AdditionalDependencies)</AdditionalDependencies>
  </Link>
</ItemDefinitionGroup>
```

- [ ] **Step 3: Create `resource.h`**

```cpp
#pragma once
#define IDI_OPENZIP_CLASSIC 101
```

- [ ] **Step 4: Create `OpenZipShellExtClassic.rc`**

```rc
#include "resource.h"
#include <windows.h>

IDI_OPENZIP_CLASSIC ICON DISCARDABLE "icon.ico"
```

Copy `src/shellext/icon.ico` to `src/shellext_classic/icon.ico`.

- [ ] **Step 5: Add to `OpenZip.sln`**

```
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "OpenZipShellExtClassic", "src\shellext_classic\OpenZipShellExtClassic.vcxproj", "{1A2B3C4D-0006-4000-8000-100000000006}"
EndProject
```
Plus the four `ProjectConfigurationPlatforms` lines (Debug/Release × ActiveCfg/Build.0).

- [ ] **Step 6: Commit (project compiles to an empty DLL — that's fine for now)**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:OpenZipShellExtClassic
```
Expected: link succeeds with stub `ClassicShellExt.cpp` (write a one-liner empty file before building):

```cpp
// ClassicShellExt.cpp
#include <Windows.h>
extern "C" BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
```

```powershell
git add src/shellext_classic/ OpenZip.sln
git commit -m "feat(shellext_classic): scaffolding for Win10 classic DLL"
```

### Task 5.2: Implement IShellExtInit + IContextMenu with Win11 OS gate

**Files:**
- Modify: `src/shellext_classic/ClassicShellExt.cpp`

- [ ] **Step 1: Replace `ClassicShellExt.cpp` with full implementation**

```cpp
#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shlwapi.h>
#include <combaseapi.h>
#include <new>
#include <string>
#include <vector>
#include <filesystem>

#include "resource.h"

namespace fs = std::filesystem;

namespace {

// {A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F2000}
constexpr GUID kCLSID_Classic = {
    0xA8F3C7E4, 0x1B2D, 0x4F5E,
    {0x9C, 0x8A, 0x3B, 0x6D, 0x2E, 0x5F, 0x20, 0x00}};

LONG g_dll_ref_count = 0;
HMODULE g_module = nullptr;

bool IsWin11OrLater() {
    using RtlGetVersionFn = LONG (WINAPI*)(OSVERSIONINFOEXW*);
    static auto fn = reinterpret_cast<RtlGetVersionFn>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!fn) return false;
    OSVERSIONINFOEXW v{}; v.dwOSVersionInfoSize = sizeof(v);
    if (fn(&v) != 0) return false;
    return v.dwBuildNumber >= 22000;
}

bool IsKoreanLocale() {
    LANGID mui = ::GetUserDefaultUILanguage();
    LANGID loc = LANGIDFROMLCID(::GetUserDefaultLCID());
    return PRIMARYLANGID(mui) == LANG_KOREAN || PRIMARYLANGID(loc) == LANG_KOREAN;
}

std::wstring AppExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(g_module, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring p(buf, n);
    auto slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    return p + L"OpenZipApp.exe";
}

void Log(const wchar_t* msg) {
    wchar_t logdir[MAX_PATH];
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, logdir))) return;
    ::wcscat_s(logdir, L"\\OpenZip");
    ::CreateDirectoryW(logdir, nullptr);
    wchar_t path[MAX_PATH]; ::wcscpy_s(path, logdir); ::wcscat_s(path, L"\\shellext.log");
    HANDLE h = ::CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    ::SetFilePointer(h, 0, nullptr, FILE_END);
    SYSTEMTIME st; ::GetLocalTime(&st);
    char line[512];
    int n = ::_snprintf_s(line, sizeof(line), _TRUNCATE,
        "[%02d:%02d:%02d.%03d pid=%lu] [classic] %ls\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ::GetCurrentProcessId(), msg);
    if (n > 0) { DWORD w; ::WriteFile(h, line, static_cast<DWORD>(n), &w, nullptr); }
    ::CloseHandle(h);
}

bool LaunchApp(const std::wstring& cmdline) {
    std::wstring c = cmdline;
    wchar_t cwd[MAX_PATH] = L"";
    ::SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, cwd);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE,
                               0, nullptr, *cwd ? cwd : nullptr, &si, &pi);
    if (ok) { ::CloseHandle(pi.hThread); ::CloseHandle(pi.hProcess); }
    return ok != FALSE;
}

class ClassicShellExt : public IShellExtInit, public IContextMenu {
public:
    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IShellExtInit) *ppv = static_cast<IShellExtInit*>(this);
        else if (riid == IID_IContextMenu) *ppv = static_cast<IContextMenu*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = ::InterlockedDecrement(&ref_); if (r == 0) delete this; return r;
    }

    // IShellExtInit — Explorer hands us the selection via CF_HDROP.
    IFACEMETHODIMP Initialize(LPCITEMIDLIST, IDataObject* dobj, HKEY) override {
        items_.clear();
        if (!dobj) return E_INVALIDARG;
        FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM stg{};
        if (FAILED(dobj->GetData(&fmt, &stg))) return E_INVALIDARG;
        HDROP hd = static_cast<HDROP>(::GlobalLock(stg.hGlobal));
        if (hd) {
            UINT n = ::DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t buf[MAX_PATH];
                if (::DragQueryFileW(hd, i, buf, MAX_PATH)) items_.emplace_back(buf);
            }
            ::GlobalUnlock(stg.hGlobal);
        }
        ::ReleaseStgMedium(&stg);
        return items_.empty() ? E_INVALIDARG : S_OK;
    }

    // IContextMenu
    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT idx, UINT idCmdFirst,
                                    UINT idCmdLast, UINT flags) override {
        if (IsWin11OrLater() || (flags & CMF_DEFAULTONLY) || items_.empty()) {
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
        }

        bool only_zip = (items_.size() == 1) && PathMatchSpecW(items_[0].c_str(), L"*.zip");
        UINT id = idCmdFirst;

        // Insert separator only if there are surrounding items.
        ::InsertMenuW(menu, idx++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);

        // Submenu: OpenZip
        HMENU sub = ::CreatePopupMenu();
        UINT sub_idx = 0;
        if (only_zip) {
            ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION,
                          id_extract_here_ = id++,
                          IsKoreanLocale() ? L"여기에 풀기" : L"Extract Here");
            std::wstring stem = fs::path(items_[0]).stem().wstring();
            std::wstring extract_to = IsKoreanLocale()
                ? L"\"" + stem + L"\\\"에 풀기"
                : L"Extract to \"" + stem + L"\\\"";
            ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION,
                          id_extract_to_folder_ = id++, extract_to.c_str());
            ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        }

        std::wstring base_name;
        if (items_.size() == 1) base_name = fs::path(items_[0]).stem().wstring();
        else base_name = fs::path(items_[0]).parent_path().filename().wstring();
        if (base_name.empty()) base_name = L"Archive";
        std::wstring t_bundle = IsKoreanLocale()
            ? L"\"" + base_name + L".zip\"으로 압축"
            : L"Compress to \"" + base_name + L".zip\"";

        ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION,
                      id_compress_bundle_ = id++, t_bundle.c_str());
        ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION,
                      id_compress_each_ = id++,
                      IsKoreanLocale() ? L"각각 압축" : L"Compress each separately");
        ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION,
                      id_compress_prompt_ = id++,
                      IsKoreanLocale() ? L"압축 옵션…" : L"Compress with options…");

        MENUITEMINFOW mii{}; mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID;
        mii.wID = id_parent_ = id++;
        mii.hSubMenu = sub;
        mii.dwTypeData = const_cast<LPWSTR>(L"OpenZip");
        ::InsertMenuItemW(menu, idx++, TRUE, &mii);

        ::InsertMenuW(menu, idx, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);

        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, id - idCmdFirst);
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* ici) override {
        if (!ici) return E_INVALIDARG;
        UINT cmd_id;
        if (HIWORD(ici->lpVerb) != 0) return E_NOTIMPL;
        cmd_id = LOWORD(ici->lpVerb);

        std::wstring exe = AppExePath();
        if (exe.empty()) return E_FAIL;
        std::wstring cmdline;

        auto first_parent = [&]() -> std::wstring {
            return items_.empty() ? L"" : fs::path(items_[0]).parent_path().wstring();
        };

        if (cmd_id == id_extract_here_) {
            cmdline = L"\"" + exe + L"\" --extract \"" + items_[0] + L"\" --here";
        } else if (cmd_id == id_extract_to_folder_) {
            cmdline = L"\"" + exe + L"\" --extract \"" + items_[0] + L"\" --folder";
        } else if (cmd_id == id_compress_bundle_) {
            std::wstring parent = first_parent();
            std::wstring base = items_.size() == 1
                ? fs::path(items_[0]).stem().wstring()
                : fs::path(parent).filename().wstring();
            cmdline = L"\"" + exe + L"\" --compress --mode bundle --output \""
                    + parent + L"\\" + base + L".zip\"";
            for (auto& p : items_) cmdline += L" --item \"" + p + L"\"";
        } else if (cmd_id == id_compress_each_) {
            cmdline = L"\"" + exe + L"\" --compress --mode each";
            for (auto& p : items_) cmdline += L" --item \"" + p + L"\"";
        } else if (cmd_id == id_compress_prompt_) {
            cmdline = L"\"" + exe + L"\" --compress --mode prompt";
            for (auto& p : items_) cmdline += L" --item \"" + p + L"\"";
        } else {
            return E_NOTIMPL;
        }

        Log(cmdline.c_str());
        return LaunchApp(cmdline) ? S_OK : E_FAIL;
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR* name, UINT cchMax) override {
        if (name && cchMax) name[0] = 0;
        return E_NOTIMPL;
    }

private:
    LONG ref_ = 1;
    std::vector<std::wstring> items_;
    UINT id_parent_ = 0, id_extract_here_ = 0, id_extract_to_folder_ = 0;
    UINT id_compress_bundle_ = 0, id_compress_each_ = 0, id_compress_prompt_ = 0;
};

class ClassFactory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = ::InterlockedDecrement(&ref_); if (r == 0) delete this; return r;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* c = new (std::nothrow) ClassicShellExt();
        if (!c) return E_OUTOFMEMORY;
        HRESULT hr = c->QueryInterface(riid, ppv);
        c->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) ::InterlockedIncrement(&g_dll_ref_count);
        else      ::InterlockedDecrement(&g_dll_ref_count);
        return S_OK;
    }
private:
    LONG ref_ = 1;
};

}  // namespace

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        ::DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != kCLSID_Classic) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) ClassFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_dll_ref_count == 0 ? S_OK : S_FALSE; }
```

- [ ] **Step 2: Build**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:OpenZipShellExtClassic
```
Expected: clean build.

- [ ] **Step 3: Commit**

```powershell
git add src/shellext_classic/ClassicShellExt.cpp
git commit -m "feat(shellext_classic): IContextMenu impl with Win11 OS gate"
```

---

## Phase 6 — MSIX manifest changes

### Task 6.1: Drop MinVersion + bump version

**Files:**
- Modify: `msix/AppxManifest.xml`

- [ ] **Step 1: Edit two attribute values**

Change:
```xml
<Identity Name="Hyung-ChulLee.OpenZip"
          Publisher="CN=F654AA56-4E26-4CE5-B9F5-949930D6F0EE"
          Version="0.2.0.0"
          ProcessorArchitecture="x64" />
```
to:
```xml
<Identity Name="Hyung-ChulLee.OpenZip"
          Publisher="CN=F654AA56-4E26-4CE5-B9F5-949930D6F0EE"
          Version="0.3.0.0"
          ProcessorArchitecture="x64" />
```

Change:
```xml
<TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.22000.0" MaxVersionTested="10.0.26100.0" />
```
to:
```xml
<TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.18362.0" MaxVersionTested="10.0.26100.0" />
```

- [ ] **Step 2: Commit**

```powershell
git add msix/AppxManifest.xml
git commit -m "build(msix): drop MinVersion to 18362, bump to 0.3.0"
```

### Task 6.2: Register classic CLSID and modern compress CLSID

**Files:**
- Modify: `msix/AppxManifest.xml`

- [ ] **Step 1: Replace the existing `windows.comServer` block with**

```xml
<com:Extension Category="windows.comServer">
  <com:ComServer>
    <com:SurrogateServer DisplayName="OpenZip Shell Extension">
      <com:Class Id="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1A0C"
                 Path="OpenZipShellExt.dll" ThreadingModel="STA" />
      <com:Class Id="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1B00"
                 Path="OpenZipShellExt.dll" ThreadingModel="STA" />
    </com:SurrogateServer>
    <com:Class Id="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F2000"
               Path="OpenZipShellExtClassic.dll" ThreadingModel="Apartment" />
  </com:ComServer>
</com:Extension>
```

- [ ] **Step 2: Commit**

```powershell
git add msix/AppxManifest.xml
git commit -m "build(msix): register classic shell ext + modern compress CLSIDs"
```

### Task 6.3: Register modern compress verbs

**Files:**
- Modify: `msix/AppxManifest.xml`

- [ ] **Step 1: Replace the existing `windows.fileExplorerContextMenus` block with**

```xml
<desktop4:Extension Category="windows.fileExplorerContextMenus">
  <desktop4:FileExplorerContextMenus>
    <desktop5:ItemType Type=".zip">
      <desktop5:Verb Id="OpenZipExtract"
                     Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1A0C" />
      <desktop5:Verb Id="OpenZipCompress"
                     Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1B00" />
    </desktop5:ItemType>
    <desktop5:ItemType Type="*">
      <desktop5:Verb Id="OpenZipCompress"
                     Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1B00" />
    </desktop5:ItemType>
    <desktop5:ItemType Type="Directory">
      <desktop5:Verb Id="OpenZipCompress"
                     Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1B00" />
    </desktop5:ItemType>
  </desktop4:FileExplorerContextMenus>
</desktop4:Extension>
```

- [ ] **Step 2: Commit**

```powershell
git add msix/AppxManifest.xml
git commit -m "build(msix): register modern compress verbs for files and folders"
```

### Task 6.4: Register classic context-menu handler

**Files:**
- Modify: `msix/AppxManifest.xml`

This is the element flagged in the spec (§11 risk #1) for fact-checking against current MSIX schema. Reference: https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop4-filexplorerclassiccontextmenuhandlers

- [ ] **Step 1: Add new extension block inside `<Extensions>` (between modern and the closing tag)**

```xml
<desktop4:Extension Category="windows.fileExplorerClassicContextMenuHandler"
                    EntryPoint="Windows.FullTrustApplication">
  <desktop4:FileExplorerClassicContextMenuHandlers>
    <desktop4:ItemType Type="*">
      <desktop4:Handler Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F2000" />
    </desktop4:ItemType>
    <desktop4:ItemType Type="Directory">
      <desktop4:Handler Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F2000" />
    </desktop4:ItemType>
    <desktop4:ItemType Type=".zip">
      <desktop4:Handler Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F2000" />
    </desktop4:ItemType>
  </desktop4:FileExplorerClassicContextMenuHandlers>
</desktop4:Extension>
```

- [ ] **Step 2: Validate with makeappx**

```powershell
& "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\makeappx.exe" pack `
    /d msix\staging /p msix\OpenZip-temp.msix /o
```
Expected: pack succeeds without "schema validation" errors. (Pre-stage some dummy files in `msix\staging` if the dir doesn't exist yet.)

If the schema is rejected, consult the MS docs cited above and update element names. The set of CLSIDs and ItemTypes is correct; only the surrounding XML naming may shift.

- [ ] **Step 3: Commit**

```powershell
git add msix/AppxManifest.xml
git commit -m "build(msix): register classic context-menu handler for Win10"
```

### Task 6.5: Update `build_msix.py` to stage the classic DLL

**Files:**
- Modify: `msix/build_msix.py`

- [ ] **Step 1: Find the existing list of binaries copied into `staging/` and add the classic DLL**

Locate the section that copies `OpenZipShellExt.dll` and add an analogous line:

```python
shutil.copy2(BUILD_DIR / "OpenZipShellExtClassic.dll",
             STAGING / "OpenZipShellExtClassic.dll")
```

- [ ] **Step 2: Run the script**

```powershell
msbuild OpenZip.sln /p:Configuration=Release /p:Platform=x64
python msix\build_msix.py 0.3.0
```
Expected: produces `msix\openzip-0.3.0.msix` (or whatever name the script uses) containing both shell-ext DLLs.

- [ ] **Step 3: Commit**

```powershell
git add msix/build_msix.py
git commit -m "build(msix): stage OpenZipShellExtClassic.dll"
```

---

## Phase 7 — Dark mode helper + apply to all dialogs

### Task 7.1: Create `dark_theme.{h,cpp}`

**Files:**
- Create: `src/app/dark_theme.h`
- Create: `src/app/dark_theme.cpp`
- Modify: `src/app/OpenZipApp.vcxproj`

- [ ] **Step 1: Create `dark_theme.h`**

```cpp
#pragma once

#include <Windows.h>

namespace openzip::dark_theme {

bool IsDarkModeActive();   // reads HKCU\…\AppsUseLightTheme; cached
void Reset();              // re-reads cache (call on WM_SETTINGCHANGE)

void EnableForWindow(HWND);                         // titlebar (Win11) + recursive children
HBRUSH OnCtlColor(HWND ctl, HDC hdc, UINT nMsg);    // returns dark brush or NULL
LRESULT OnProgressCustomDraw(NMHDR* hdr);            // for CProgressCtrl NM_CUSTOMDRAW

}  // namespace openzip::dark_theme
```

- [ ] **Step 2: Create `dark_theme.cpp`**

```cpp
#include "dark_theme.h"

#include <dwmapi.h>
#include <Uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace openzip::dark_theme {

namespace {
constexpr COLORREF kBgDark   = RGB(32, 32, 32);
constexpr COLORREF kFgDark   = RGB(220, 220, 220);
constexpr COLORREF kEditDark = RGB(45, 45, 45);

bool g_cached_valid = false;
bool g_cached = false;
HBRUSH g_brush_bg = nullptr;
HBRUSH g_brush_edit = nullptr;

bool ReadAppsUseLightTheme() {
    HKEY k;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &k) != ERROR_SUCCESS) return true;
    DWORD val = 1, sz = sizeof(val), type = 0;
    LSTATUS s = ::RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, &type,
                                   reinterpret_cast<LPBYTE>(&val), &sz);
    ::RegCloseKey(k);
    if (s != ERROR_SUCCESS || type != REG_DWORD) return true;
    return val != 0;
}

void EnsureBrushes() {
    if (!g_brush_bg)   g_brush_bg   = ::CreateSolidBrush(kBgDark);
    if (!g_brush_edit) g_brush_edit = ::CreateSolidBrush(kEditDark);
}

}  // namespace

bool IsDarkModeActive() {
    if (!g_cached_valid) {
        g_cached = !ReadAppsUseLightTheme();
        g_cached_valid = true;
    }
    return g_cached;
}

void Reset() { g_cached_valid = false; }

void EnableForWindow(HWND hwnd) {
    if (!IsDarkModeActive()) return;
    EnsureBrushes();

    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                            &dark, sizeof(dark));

    ::EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
        wchar_t cls[64]{};
        ::GetClassNameW(child, cls, 64);
        if (lstrcmpiW(cls, L"Button") == 0 || lstrcmpiW(cls, L"ComboBox") == 0)
            ::SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
        else if (lstrcmpiW(cls, L"Edit") == 0)
            ::SetWindowTheme(child, L"DarkMode_CFD", nullptr);
        else if (lstrcmpiW(cls, L"SysListView32") == 0)
            ::SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
        return TRUE;
    }, 0);
}

HBRUSH OnCtlColor(HWND, HDC hdc, UINT nMsg) {
    if (!IsDarkModeActive()) return nullptr;
    EnsureBrushes();
    ::SetTextColor(hdc, kFgDark);
    if (nMsg == WM_CTLCOLOREDIT) {
        ::SetBkColor(hdc, kEditDark);
        return g_brush_edit;
    }
    ::SetBkColor(hdc, kBgDark);
    return g_brush_bg;
}

LRESULT OnProgressCustomDraw(NMHDR* hdr) {
    auto* nmcd = reinterpret_cast<LPNMCUSTOMDRAW>(hdr);
    if (nmcd->dwDrawStage == CDDS_PREERASE) {
        if (!IsDarkModeActive()) return CDRF_DODEFAULT;
        EnsureBrushes();
        ::FillRect(nmcd->hdc, &nmcd->rc, g_brush_bg);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

}  // namespace openzip::dark_theme
```

- [ ] **Step 3: Register in `OpenZipApp.vcxproj`**

```xml
<ClCompile Include="dark_theme.cpp" />
<ClInclude Include="dark_theme.h" />
```

- [ ] **Step 4: Build**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64 /t:OpenZipApp
```
Expected: clean build.

- [ ] **Step 5: Commit**

```powershell
git add src/app/dark_theme.* src/app/OpenZipApp.vcxproj
git commit -m "feat(app): dark theme helper module"
```

### Task 7.2: Apply dark theme to `CExtractDialog`

**Files:**
- Modify: `src/app/ExtractDialog.cpp`
- Modify: `src/app/ExtractDialog.h`

- [ ] **Step 1: Add include and message map entries to the header**

In `ExtractDialog.h` near the top:
```cpp
#include "dark_theme.h"
```
In the `protected:` section, add:
```cpp
afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
```

- [ ] **Step 2: Wire up in `ExtractDialog.cpp`**

In the existing `OnInitDialog` (after `CDialogEx::OnInitDialog();`):
```cpp
openzip::dark_theme::EnableForWindow(GetSafeHwnd());
```

Add to `BEGIN_MESSAGE_MAP`:
```cpp
ON_WM_CTLCOLOR()
```

Add the method body:
```cpp
HBRUSH CExtractDialog::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
    if (HBRUSH b = openzip::dark_theme::OnCtlColor(
            pWnd->GetSafeHwnd(), pDC->GetSafeHdc(),
            (nCtlColor == CTLCOLOR_EDIT) ? WM_CTLCOLOREDIT : WM_CTLCOLORDLG))
        return b;
    return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
}
```

- [ ] **Step 3: Build, sideload, and visually inspect**

```powershell
msbuild OpenZip.sln /p:Configuration=Debug /p:Platform=x64
python msix\build_msix.py 0.3.0
Add-AppxPackage -Register .\msix\staging\AppxManifest.xml
# Right-click any .zip in Explorer → "여기에 풀기" → confirm dark theme applied
```

(Toggle Win11 to dark mode in Settings → Personalization → Colors first.)

- [ ] **Step 4: Commit**

```powershell
git add src/app/ExtractDialog.* 
git commit -m "feat(app): apply dark theme to CExtractDialog"
```

### Task 7.3–7.6: Apply dark theme to remaining dialogs

For each of `CPasswordDialog`, `CConflictDialog`, `CCompressOptionsDialog`, `CCompressDialog`, repeat the same three-step pattern as Task 7.2:

- [ ] **Apply to `CPasswordDialog` (commit per file)**
- [ ] **Apply to `CConflictDialog`**
- [ ] **Apply to `CCompressOptionsDialog`**
- [ ] **Apply to `CCompressDialog`** (also wire `NM_CUSTOMDRAW` for the progress bar)

For the progress bar custom-draw, in `CCompressDialog.cpp`:
```cpp
ON_NOTIFY(NM_CUSTOMDRAW, IDC_COMPRESS_PROGRESS, &CCompressDialog::OnProgressCustomDraw)

LRESULT CCompressDialog::OnProgressCustomDraw(NMHDR* hdr, LRESULT* result) {
    *result = openzip::dark_theme::OnProgressCustomDraw(hdr);
    return 0;
}
```

After all four:
```powershell
git log --oneline -5  # confirm 4 dark-theme commits
```

---

## Phase 8 — Validation, docs, release

### Task 8.1: Win11 24H2 sideload validation

- [ ] **Step 1: Build release MSIX**

```powershell
msbuild OpenZip.sln /p:Configuration=Release /p:Platform=x64
python msix\build_msix.py 0.3.0
```

- [ ] **Step 2: Sideload on Win11 24H2 (developer mode + Add-AppxPackage)**

```powershell
Add-AppxPackage -Register .\msix\staging\AppxManifest.xml
```

- [ ] **Step 3: Walk the §12 DoD acceptance list for Win11 items**

Manually verify each Win11 checkbox in the spec §12. Any failure → file fix, return to relevant phase.

### Task 8.2: Win10 22H2 sideload validation

- [ ] **Step 1: Sideload on a Win10 22H2 VM (same `Add-AppxPackage` command)**

- [ ] **Step 2: Walk the §12 Win10 acceptance items**

In particular: confirm the classic shell ext is in-process to explorer (`tasklist /M OpenZipShellExtClassic.dll` from cmd). If Win10 sideload fails for trust reasons, follow the VR project's documented procedure (from user memory) — typically `Add-AppxPackage -ForceApplicationShutdown` plus a self-signed cert if needed.

### Task 8.3: Win10 1903 floor validation

- [ ] **Step 1: Sideload on a Win10 1903 VM (the MinVersion floor)**

- [ ] **Step 2: Smoke test the OpenZip submenu appears and "Compress to <name>.zip" produces a valid archive**

If 1903 fails (it has the most marginal MSIX shell-ext support), bump `MinVersion` to `10.0.19041.0` (Win10 2004) and re-test, then update the spec's §3 #1 decision retroactively.

### Task 8.4: Update README

- [ ] **Step 1: Edit `README.md` Features section**

Add the new bullets:
- Compression with the same correctness guarantees (Korean filenames done right)
- Windows 10 (1903+) support via classic context menu
- Dark mode follows OS theme

- [ ] **Step 2: Refresh screenshots**

Replace `docs/screenshots/context-menu.png` with one showing both extract and compress entries. Add `docs/screenshots/compress-options.png` and `docs/screenshots/dark-mode.png`.

- [ ] **Step 3: Commit**

```powershell
git add README.md docs/screenshots/
git commit -m "docs: README + screenshots for v0.3 (compress, Win10, dark mode)"
```

### Task 8.5: Microsoft Store submission

- [ ] **Step 1: Tag the release**

```powershell
git tag v0.3.0
git push --tags
```

- [ ] **Step 2: Upload `msix\openzip-0.3.0.msix` to Partner Center, fill release notes, submit for certification**

Have justification text ready for the classic shell extension if reviewers ask: "Required for Win10 file-context-menu integration of the same operations Win11 already supports through the modern shell."

- [ ] **Step 3: Update memory after release**

When the Store cert clears, save a feedback memory if the classic-shell-ext review took notable time / required justification correspondence — useful for future MSIX submissions of the same author.

---

## Self-Review Notes

Spec coverage check (against `2026-05-02-compress-and-win10-menu-design.md`):

- §3 decisions 1–10 → covered in Phases 0–8 (1 → 6.1; 2 → 5.x; 3 → 4.x unchanged; 4 → 5.2 OS gate; 5 → MFC retained throughout; 6 → 4.1+5.2; 7 → 4.2+5.2 `LaunchAppCompress`; 8 → 1.4–1.6 + 3.1; 9 → 6.1; 10 → entire plan structure)
- §4 binary inventory → all five entries created/modified
- §5 Compressor → Phase 1 (Tasks 1.1–1.9) covers atomic output, encoding, AES, callbacks, conflict, byte progress
- §6 App UI → Phase 3 (Tasks 3.1–3.5), MFC retained
- §6.4 dark mode → Phase 7
- §7 shell extensions → Phases 4 (modern) + 5 (classic), both with same command-line contract
- §8 manifest → Phase 6
- §9 testing → Phase 1 unit tests + Phase 8 manual validation
- §11 risks → addressed inline (risk #1 in 6.4, #4 in classic DLL minimal scope, #5 in 8.5 messaging)
- §12 DoD → Task 8.1–8.3 walks through the checkboxes

No placeholder strings remain. Type names consistent across tasks (`Compressor::Compress`, `Compressor::ProgressCallback`, `Extractor::ConflictAction` reused everywhere). Method signatures in Phase 3 dialog code match the public surface declared in Phase 1 `compressor.h`.
