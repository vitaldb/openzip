# OpenZip — Implementation Plan (v2)

> A minimal Windows 11 ZIP extractor with Explorer context menu integration, built with MFC.
> Distributed via Microsoft Store as MSIX. Open source under MIT.

## 1. Project Goal

Build a lightweight ZIP-only extractor for Windows 11 that integrates into the Explorer context menu, similar to Bandizip's "Extract Here" / "Extract to folder" workflow.

**Repository:** https://github.com/vitaldb/openzip
**License:** MIT (wrapper code); third-party libs keep their own licenses.
**Distribution:** Microsoft Store (Store handles signing). No standalone installer.

**Non-goals (explicitly out of scope for v1):**
- Compression (extract only)
- Formats other than `.zip` (no RAR, 7z, TAR, etc.)
- Windows 10 support (Win11 only — modern context menu is Win11-only anyway)
- Archive browsing / preview UI
- Drag-and-drop into a window
- Auto-update logic (Store handles updates)
- Telemetry, network calls, crash reporting

## 2. Constraints & Environment

- **OS target:** Windows 11 (build 22000+) — `MinVersion="10.0.22000.0"` in manifest
- **Language:** C++17, **MFC linkage: Static** (matches VitalRecorder convention)
- **Architecture:** x64 only
- **Distribution:** MSIX submitted to Microsoft Store under existing Hyung-Chul Lee publisher
  - Identity: `Hyung-ChulLee.OpenZip`
  - Publisher: `CN=F654AA56-4E26-4CE5-B9F5-949930D6F0EE` (same Store account as VR)
  - **No local signing** — Store signs the package on submission
- **Build system:** MSBuild + vcpkg (manifest mode), Python script for MSIX packaging (not WAP project)
- **No telemetry, no network calls, no auto-update**

## 3. Architecture Overview

Three components in one Visual Studio solution + standalone msix/ packaging directory:

```
OpenZip.sln
├── OpenZipCore/        Static lib — ZIP extraction engine (wraps minizip-ng)
├── OpenZipApp/         MFC EXE — extraction UI, progress, password prompt
└── OpenZipShellExt/    Win32 DLL — IExplorerCommand for Win11 modern context menu
msix/
├── AppxManifest.xml    Hand-managed manifest
├── Assets/             Logos at all required sizes
└── build_msix.py       Stages files + runs makeappx pack
```

### Why this split

- **Core as static lib:** keeps extraction logic testable and independent of UI/COM. The EXE links it directly; no separate DLL to ship.
- **App as MFC EXE:** the actual "extractor" the user sees. Invoked from context menu OR by double-clicking a .zip if user sets it as default.
- **Shell extension as separate DLL:** Win11 modern context menu requires `IExplorerCommand` implemented in a packaged COM server. Must be a separate binary registered via `AppxManifest.xml`.

### Why msix/ + Python (not WAP project)

- Manifest is plain text in git, clean diffs
- VR uses this exact pattern and it works
- VS-independent — CI/headless build works
- WAP only adds dependency-tracking which we don't need (we're already explicit)

### Data flow

```
User right-clicks .zip in Explorer
    ↓
Win11 shell loads OpenZipShellExt (IExplorerCommand)
    ↓
User selects "Extract Here" or "Extract to <foldername>\"
    ↓
Shell extension launches OpenZipApp.exe with args:
    OpenZipApp.exe --extract <zip-path> --target <output-dir> [--here|--folder]
    ↓
OpenZipApp checks single-instance mutex — if another instance running,
forwards args via named pipe and exits; else runs the extraction
    ↓
OpenZipApp shows progress dialog, calls OpenZipCore on worker thread
    ↓
OpenZipCore uses minizip-ng to extract entries one by one,
posts progress messages back to UI thread
    ↓
On completion: dialog closes (or shows error / summary)
```

## 4. Third-Party Dependencies

| Library | Purpose | License | Linkage |
|---|---|---|---|
| **minizip-ng** | ZIP read + decompression, password support, AES | zlib | static |
| **zlib-ng** (transitive) | DEFLATE | zlib | static |

**Acquisition:** vcpkg in manifest mode (`vcpkg.json` at repo root). This gives reproducible builds without committing binaries.

```json
{
  "name": "openzip",
  "version-string": "0.1.0",
  "dependencies": [
    { "name": "minizip-ng", "features": ["zlib", "openssl"] }
  ]
}
```

`openssl` feature is needed for AES-256 encrypted ZIP support.

**Do NOT use:**
- `IShellDispatch::NameSpace` (Windows built-in ZIP) — broken for CP949 filenames, can't handle passwords, slow
- libzip — works fine but minizip-ng has better Korean filename heuristics
- 7-Zip's library — overkill for ZIP-only

## 5. Korean Filename Handling — Critical Detail

This is the single most important correctness issue. Most Korean ZIP archives created by older WinZip/Bandizip versions use **CP949 (EUC-KR)** filenames without the UTF-8 flag set. Naive extractors produce mojibake (깨진 한글).

**Robust decoding strategy** (in `core/filename_decoder.cpp`):

```
function decode_filename(raw_bytes, utf8_flag):
    if utf8_flag is set:
        return UTF8_decode(raw_bytes)               # trust the flag

    if all bytes are ASCII (< 0x80):
        return ASCII_decode(raw_bytes)              # unambiguous

    # Has non-ASCII bytes, no flag → could be UTF-8 or CP949
    if UTF8_strict_decode(raw_bytes) succeeds:
        # Modern tools sometimes write UTF-8 without setting flag
        # but CP949 byte sequences rarely produce valid UTF-8 by accident
        return UTF8 result

    if CP949_decode(raw_bytes) succeeds:
        decoded = CP949_to_UTF16(raw_bytes)
        if any codepoint in Hangul BMP range (AC00-D7AF, 1100-11FF, 3130-318F):
            return decoded                          # confident it's Korean
        return decoded                              # CP949 ASCII subset etc.

    return CP437_decode(raw_bytes)                  # last-resort OEM fallback
```

minizip-ng exposes the raw filename bytes; we do the decoding ourselves. **Do not trust minizip-ng's default conversion.**

**Test corpus** (`tests/test_archives/`): collect real archives covering each branch of the decoder. CI must run filename decoder unit tests on every push.

## 6. Phase Breakdown

### Phase 0 — Repo + packaging skeleton (~1h)

**Why this is Phase 0, not part of Phase 4:** Shell extension (Phase 3) only loads from a registered packaged app. Without msix infrastructure ready, you can't test Phase 3 at all. So we set up the skeleton now and grow into it.

Tasks:
1. `git init`, push to https://github.com/vitaldb/openzip
2. Add: `LICENSE` (MIT), `README.md` (EN+KR), `.gitignore`, `vcpkg.json`
3. Create dir skeleton: `src/{core,app,shellext}`, `msix/{Assets}`, `tests/test_archives/`, `tools/cli_test/`
4. Empty `OpenZip.sln` with three placeholder projects (each builds to no-op)
5. `msix/AppxManifest.xml` based on VR's, with placeholder asset paths
6. `msix/build_msix.py` adapted from VR's
7. `msix/generate_assets.py` adapted from VR's (using a placeholder icon for now)
8. `.github/workflows/build.yml` — vcpkg + msbuild, uploads `.msix` artifact (no signing)

**Acceptance:** `python msix/build_msix.py` produces a valid (unsigned) `.msix` containing the placeholder DLL/EXE files. `Add-AppxPackage -Register .\msix\staging\AppxManifest.xml` in Developer Mode installs successfully.

### Phase 1 — Core extraction (~4-5h)

**Deliverable:** Console test harness that extracts any ZIP with correct Korean filenames.

Tasks:
1. `OpenZipCore` static lib with this surface:
   ```cpp
   class Extractor {
   public:
     struct Entry { std::wstring name; uint64_t size; bool isDir; bool needsPassword; };
     enum class ConflictAction { Overwrite, Skip, Rename, Cancel };
     struct ProgressCallback {
       virtual void OnEntry(const Entry&, uint64_t bytesDone, uint64_t totalBytes) = 0;
       virtual std::wstring OnPasswordRequired(const std::wstring& archiveName) = 0;
       virtual ConflictAction OnFileConflict(const std::wstring& path) = 0;
       virtual bool ShouldCancel() = 0;
     };
     enum class Result { Success, Cancelled, BadPassword, IoError, CorruptArchive,
                         BombRefused, UnsafePath, ReservedName };
     Result Extract(const std::filesystem::path& zip,
                    const std::filesystem::path& targetDir,
                    ProgressCallback& cb);
     std::vector<Entry> ListEntries(const std::filesystem::path& zip);
   };
   ```
2. `core/filename_decoder.{h,cpp}` per §5
3. `core/path_validator.cpp`:
   - Zip-slip protection (normalized path must stay under target)
   - Symlink entry rejection
   - Windows reserved name handling (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`, trailing `.`/space)
   - Long path support (`\\?\` prefix when path > MAX_PATH)
4. **Decompression bomb guard** (in v1, not deferred): refuse if uncompressed total > 100× archive size AND > 10GB unless caller acknowledges
5. Console test app `tools/cli_test.exe` taking `<zip> [<target>]`
6. **Test against:**
   - Modern UTF-8 zip (Win11 native or recent 7-Zip)
   - Legacy CP949 zip (old WinZip on Korean Windows)
   - Password-protected zip (ZipCrypto)
   - Password-protected zip (AES-256, WinZip-style)
   - Zip with directory traversal (`../../../etc/passwd`) — must reject with `UnsafePath`
   - Zip with reserved name (`CON.txt`) — must reject or sanitize
   - Empty zip
   - Zip with 10,000+ small files
   - Single-file zip > 4GB (Zip64)

**Acceptance:** all 9 test cases extract correctly or fail safely; unit tests pass with /W4.

### Phase 2 — MFC GUI (~4-5h)

**Deliverable:** `OpenZipApp.exe` that takes command-line args and shows progress.

Tasks:
1. MFC dialog-based application skeleton (Static MFC linkage)
2. Command-line parser: `--extract <zip>`, `--target <dir>`, `--here`, `--folder`, `--password <pw>`
3. **Single-instance + queue** (in v1):
   - Named mutex `OpenZipApp.SingleInstance.<sessionid>` via `CreateMutex`
   - First instance creates a named pipe `\\.\pipe\OpenZipApp.<sessionid>` and processes a queue
   - Subsequent instances write their args to the pipe and exit immediately
   - Prevents disk thrash on bulk-extract (right-click 5 zips at once)
4. `CExtractDialog`:
   - Current file label (with ellipsized path for long names)
   - Overall progress bar (% of total bytes)
   - Current file progress bar
   - Speed indicator (MB/s, smoothed over last 1s)
   - Cancel button
   - Queue indicator if multiple zips pending
5. `CPasswordDialog` — modal, returns password or cancellation
6. `CConflictDialog` — Overwrite / Skip / Rename / **default focus = Skip** to prevent accidental overwrites; checkbox "Apply to all in this archive"
7. Worker thread (`AfxBeginThread`) running `Extractor::Extract`, posts `WM_USER+N` messages
8. Result dialog on completion (success summary or error detail)

**Important UI details:**
- System DPI awareness (Win11 mixed-DPI is common)
- High contrast theme support
- Korean and English string tables in `.rc`
- Don't show "always on top"
- **"Open destination after extraction"** option, **default OFF** (annoying for batch extract)

**Acceptance:** `OpenZipApp.exe --extract test.zip --target C:\temp\out` shows working progress dialog and completes correctly. Bulk-launch 5 instances → only 1 process, others queue.

### Phase 3 — Shell extension (~3-4h)

**Deliverable:** Right-click on .zip in Explorer shows "OpenZip" submenu in modern (not legacy) context menu.

Tasks:
1. `OpenZipShellExt` Win32 DLL project (NOT MFC — keep it minimal)
2. Implement `IExplorerCommand` interface — modern Win11 way. **Do NOT implement legacy `IContextMenu`/`IShellExtInit`** for v1; those go into the "Show more options" submenu and are not visible in the modern menu.
3. The COM class is registered via the **MSIX AppxManifest**, NOT via `regsvr32`. Win11's modern menu only loads packaged COM servers.

The `AppxManifest.xml` extension block:
```xml
<Extensions>
  <com:Extension Category="windows.comServer">
    <com:ComServer>
      <com:SurrogateServer DisplayName="OpenZip Shell">
        <com:Class Id="<GUID>" Path="OpenZipShellExt.dll" ThreadingModel="STA"/>
      </com:SurrogateServer>
    </com:ComServer>
  </com:Extension>
  <desktop4:Extension Category="windows.fileExplorerContextMenus">
    <desktop4:FileExplorerContextMenus>
      <desktop5:ItemType Type=".zip">
        <desktop5:Verb Id="OpenZip" Clsid="<GUID>"/>
      </desktop5:ItemType>
    </desktop4:FileExplorerContextMenus>
  </desktop4:Extension>
</Extensions>
```

4. Implement `IExplorerCommand` methods:
   - `GetTitle` — "OpenZip" (parent), with subcommands "Extract Here" and "Extract to <name>\"
   - `GetIcon` — points to a resource ID in the DLL
   - `GetState` — return `ECS_ENABLED` if selection is exactly one .zip
   - `Invoke` — launch `OpenZipApp.exe` via `CreateProcessW` (resolve path via `GetModuleFileName` + relative path, since packaged app install location is dynamic)
   - `EnumSubCommands` — for parent/child menu structure
5. Subcommands (Bandizip behavior):
   - **Extract Here** → extracts into the directory containing the .zip (no subfolder)
   - **Extract to "filename\\"** → extracts into a new subfolder named after the zip (without .zip extension)
   - **Smart-extract bonus:** when "Extract to filename\" is invoked, if the zip's top-level entries all share a common root folder matching the zip name, skip creating the redundant outer folder

**Critical gotchas:**
- DLL must export `DllGetClassObject`, `DllCanUnloadNow`. **Must NOT** export `DllRegisterServer` (MSIX registration replaces that).
- Win11 caches shell extensions aggressively. During dev, restart explorer:
  `taskkill /f /im explorer.exe; start explorer`
- **Always test from installed package**, not from VS Run.
- Modern context menu fires only on **packaged** apps — Phase 0 packaging is a hard prerequisite.

**Acceptance:** Right-click any .zip → modern context menu shows OpenZip entries → clicking launches the app and extracts correctly.

### Phase 4 — MSIX submission (~1-2h once Phase 3 is solid)

**Deliverable:** Microsoft Store-submitted `.msix`, awaiting cert.

Tasks:
1. In Microsoft Partner Center: reserve product name "OpenZip" under Hyung-Chul Lee publisher account
2. Confirm Publisher CN matches between Partner Center and `AppxManifest.xml`
3. Run `python msix/build_msix.py 0.1.0`
4. Upload `.msix` to Partner Center
5. Fill out Store listing (description in EN+KR, screenshots, age rating, privacy policy = "no data collected")
6. Submit for certification
7. **Sideload-test the same .msix on a clean Win11 VM** before submitting

**Store cert risk:** First-time shell context menu submission may take 1-3 business days; Store reviewers sometimes flag `runFullTrust` extensions. Worst case: respond with justification ("required for IExplorerCommand ZIP extraction"). Hyung-Chul Lee account is already trusted via VR, so faster turnaround is likely.

**Acceptance:** App appears in Store, installs cleanly, all features work, no warnings.

### Phase 5 — Polish (post-v1, optional)

### Done in v0.2.0
- **Parallel extraction** — `Extractor::Options::concurrency` (auto = `hardware_concurrency` clamped to [1, 16]). Encrypted archives stay sequential to preserve password-retry semantics. Tiny archives (< 4 entries) also stay sequential. Conflict callback in CExtractDialog is now `std::mutex`-serialized so concurrent workers can't stack overlapping prompts.
- Measured on a 32-core dev box (auto vs `--threads 1`):
  - 100 × 1 MB random-data archive: 654 ms → 492 ms (~1.3×, write-throughput limited)
  - 5000-tiny-file archive: 6.8 s → 3.5 s (~2×, NTFS metadata dominates)
- Real-world Korean text-heavy ZIPs should see better gains since DEFLATE actually does CPU work.

### Phase 5 polish — remaining

In priority order:

1. **Logging** — `%LOCALAPPDATA%\OpenZip\logs\` for user bug reports
2. **Settings file** — `%LOCALAPPDATA%\OpenZip\config.json`: default conflict action, last-used target dir, language preference
3. **Full Korean localization** (en-US default, ko-KR translation)
4. **Designed icons** (replace placeholders)
5. **Progress dialog refinement**
6. **System sound on completion** (option, default off)
7. **Win11 dark mode support** — MFC dialogs don't get this for free. Two paths:
   (a) immersive title bar via `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` — 5 min, but mixed-theme look (dark titlebar + light content) is worse than fully light. Skip.
   (b) Subclass `CButton`/`CEdit`/`CStatic`/`CProgressCtrl` with custom `OnPaint` against the system theme. ~1-2 days. Worth it only after Store launch + user feedback shows demand.
   Reference: 7-Zip ships light-only at 25 years; Bandizip has full custom-painted UI. We're closer to 7-Zip.

## 7. Repository Layout

```
openzip/
├── README.md                  EN + KR
├── LICENSE                    MIT
├── .gitignore
├── vcpkg.json
├── OpenZip.sln
├── .github/
│   └── workflows/
│       └── build.yml          Build + .msix artifact (no sign)
├── src/
│   ├── core/                  OpenZipCore static lib
│   │   ├── extractor.{h,cpp}
│   │   ├── filename_decoder.{h,cpp}
│   │   └── path_validator.{h,cpp}
│   ├── app/                   OpenZipApp MFC EXE
│   │   ├── stdafx.h
│   │   ├── OpenZipApp.cpp
│   │   ├── ExtractDialog.{h,cpp}
│   │   ├── PasswordDialog.{h,cpp}
│   │   ├── ConflictDialog.{h,cpp}
│   │   ├── SingleInstance.{h,cpp}
│   │   └── res/
│   └── shellext/              OpenZipShellExt DLL
│       ├── dllmain.cpp
│       ├── ExplorerCommand.{h,cpp}
│       └── res/
├── msix/
│   ├── AppxManifest.xml
│   ├── Assets/
│   ├── build_msix.py
│   └── generate_assets.py
├── tests/
│   ├── test_archives/         (small sample zips covering edge cases)
│   ├── extractor_tests.cpp
│   └── filename_decoder_tests.cpp
└── tools/
    └── cli_test/              console harness from Phase 1
```

## 8. Security Considerations (all in v1)

1. **Zip-slip protection** — refuse any entry whose normalized path escapes target dir. Unit-tested.
2. **Symlink entries** — refuse to create symlinks during extraction.
3. **Decompression bombs** — refuse if uncompressed > 100× archive size AND > 10GB. (No user override in v1; just refuse.)
4. **Long path handling** — `\\?\` prefix when paths exceed MAX_PATH.
5. **Don't follow junctions/symlinks** in target dir when checking conflicts.
6. **Reserved Windows names** — sanitize or refuse `CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`, names ending in `.` or space.

## 9. Things That Will Probably Bite You

In rough order of likelihood:

1. **Shell extension caching** — changes don't appear until explorer restart. Add a `tools/restart_explorer.bat`.
2. **MSIX Identity mismatch** — Identity Name + Publisher CN in AppxManifest must exactly match Partner Center reservation. Mismatch → submission rejected.
3. **MFC + vcpkg manifest mode** — VS sometimes doesn't pick up the manifest. Set `VcpkgEnableManifest=true` in project properties explicitly.
4. **CP949 detection edge cases** — see §5 algorithm. Trust the heuristic order: UTF-8 flag > strict UTF-8 decode > CP949 with Hangul confidence > CP437.
5. **IExplorerCommand only fires on packaged apps** — testing without MSIX install will silently do nothing. Always test from installed package.
6. **Win11 22H2 vs 23H2 vs 24H2** — modern context menu API has minor differences. Test on at least 24H2.
7. **Cancellation race** — partial files exist if user cancels mid-extract. Policy: **delete partial file of the entry being written** but leave already-completed entries. Document in README.
8. **Store cert review on shell extensions** — first submission takes longer; have justification ready for `runFullTrust`.
9. **Static MFC + minizip-ng linkage** — minizip-ng vcpkg port may default to dynamic CRT. Ensure both link against `/MT` (static CRT) to match Static MFC.

## 10. Out of Scope — Possibly Future

For Claude Code's awareness; do **not** implement these in v1:

- Creating ZIP files (compression)
- RAR/7z/TAR support
- Archive preview without extraction
- Drag-and-drop UI
- Multi-volume archives
- ZIP comments display/edit
- Self-extracting archive creation

## 11. Definition of Done for v0.1.0

The following must all be true:

- [ ] Right-click any `.zip` on Win11 24H2 → "OpenZip" in modern (not legacy) context menu
- [ ] "Extract Here" works for UTF-8 and CP949 archives, no mojibake
- [ ] "Extract to filename\" creates subfolder per Bandizip semantics (with smart-extract dedup)
- [ ] Password prompt appears for encrypted archives (ZipCrypto + AES-256)
- [ ] Cancel button stops extraction within 1 second; partial-file policy documented
- [ ] Conflict dialog defaults to Skip; "Apply to all" works per-archive
- [ ] Single-instance queueing prevents thrash when extracting 5+ zips simultaneously
- [ ] Zip-slip, symlink, decompression-bomb, and reserved-name protections unit-tested
- [ ] MSIX installs and uninstalls cleanly (no leftover entries)
- [ ] Solution builds clean with /W4, no warnings
- [ ] Console test harness covers all 9 test cases from Phase 1
- [ ] Submitted to Microsoft Store

## 12. Decisions Locked In

| # | Decision | Choice |
|---|---|---|
| 1 | MFC linkage | **Static** (matches VR) |
| 2 | C++ standard | **C++17** |
| 3 | Code signing | **Not needed locally** — Store signs |
| 4 | Default conflict action | **Prompt user** with "remember choice" checkbox |
| 5 | Extract Here semantics | **Bandizip style** — same dir as zip; "Extract to foo\" creates subfolder |
| 6 | "Open destination after extraction" | Option, **default OFF** |
| 7 | Default-focused button on Conflict dialog | **Skip** (prevent accidental overwrite) |
| 8 | Partial file policy on cancel | **Delete entry being written**, keep completed entries |
| 9 | Decompression bomb threshold | **100× ratio AND >10GB** → refuse |
| 10 | Identity / Publisher | `Hyung-ChulLee.OpenZip` / `CN=F654AA56-4E26-4CE5-B9F5-949930D6F0EE` |
| 11 | MinVersion | `10.0.22000.0` (Win11 21H2) |
| 12 | GitHub repo | https://github.com/vitaldb/openzip |
