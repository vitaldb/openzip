# OpenZip v0.3 — Compression + Windows 10 Context Menu — Design

> Status: design (approved 2026-05-02). Implementation plan to follow in a separate document.

## 1. Goal

Add two capabilities to OpenZip v0.3 while preserving the project's "minimal, fast, Korean ZIP done right" identity:

1. **Compress from Explorer** — right-click on selected files/folders → create a `.zip`.
2. **Windows 10 support** — show OpenZip's commands in Win10's classic right-click menu (the only menu Win10 has).

Single MSIX continues to be the only distribution channel. Win11 keeps the modern context menu it has today; Win10 sees only the new classic menu.

## 2. Non-goals (still out of scope in v0.3)

- Formats other than `.zip` (no 7z, RAR, TAR).
- Compression algorithms beyond DEFLATE.
- Split / multi-volume archives.
- Self-extracting archives (SFX).
- Solid compression.
- "Compress and email", cloud upload, telemetry.
- Replacing MFC with pure Win32 (decided against — see §10 Decisions Locked In).

## 3. Decisions Locked In

| # | Decision | Choice |
|---|---|---|
| 1 | OS coverage | Win10 1903+ (`MinVersion=10.0.18362.0`) and Win11 |
| 2 | Win10 menu mechanism | Classic `IContextMenu`+`IShellExtInit` shell extension |
| 3 | Win11 menu mechanism | Existing modern `IExplorerCommand` (no change) |
| 4 | Coexistence | Classic shell ext detects Win11 at runtime and returns 0 menu items, so Win11 users never see classic OpenZip entries (not even in "Show more options") |
| 5 | UI framework | **MFC retained.** Dark mode implemented directly via custom helpers — same effort as pure Win32, lower migration cost |
| 6 | Compress menu structure | Parent "OpenZip" with three sub-verbs: `<name>.zip`으로 압축 / 각각 압축 / 압축 옵션… |
| 7 | Multi-selection name (sub-verb 1) | Use the parent folder's name (e.g., 5 files in `Documents\` → `Documents.zip`) |
| 8 | Compress options dialog scope | Minimal — output filename/location, compression level (Store/Fast/Normal/Max, default Normal), password (empty = plaintext, non-empty = AES-256 automatically) |
| 9 | Distribution | Same MSIX, same Microsoft Store listing, same publisher; bump version to 0.3.0 |
| 10 | Implementation strategy | "Symmetric expansion" — extend each existing component with a compressor counterpart |

## 4. Component Inventory & Data Flow

### 4.1 Binary inventory

| Component | Type | v0.2 → v0.3 |
|---|---|---|
| `OpenZipCore` | static lib | Add `Compressor` class next to `Extractor`. Reuse `path_validator` + `filename_decoder` |
| `OpenZipApp` | MFC EXE (Static MFC, x64) | Add `--compress` mode, `CCompressOptionsDialog`, `CCompressDialog`. Apply dark-mode helper to all dialogs (existing 3 + new 2) |
| `OpenZipShellExt` | Win32 DLL (modern, existing) | Add 4 compress verbs (1 parent + 3 children). Existing `.zip` extract verbs unchanged |
| **`OpenZipShellExtClassic`** | **Win32 DLL (classic, NEW)** | `IContextMenu`+`IShellExtInit`. Renders extract+compress entries in Win10's classic menu. On Win11, returns 0 items |
| `msix/AppxManifest.xml` | manifest | `MinVersion` ↓ `10.0.18362.0`. Register classic DLL CLSID. Register classic context-menu handler. Register modern compress verbs |

### 4.2 Compress data flow

```
User selects N items in Explorer → right-click → "OpenZip" submenu
   ↓
Modern shell ext (Win11) or Classic shell ext (Win10) collects N paths
   ↓
Spawn: OpenZipApp.exe --compress --output "<dir>\Documents.zip"
                     --mode {bundle|each|prompt}
                     [--level normal] [--password ...] [--encoding utf8]
                     --item "C:\path\a.txt" --item "C:\path\b.txt" ...
   ↓
OpenZipApp single-instance gate (existing mutex+pipe). If another instance
running, args forwarded to it; otherwise this instance becomes the queue host
   ↓
mode=prompt → CCompressOptionsDialog first, then proceed as bundle
mode=bundle → 1 job pushed to queue
mode=each   → N jobs pushed to queue (one per item)
   ↓
Queue worker thread runs Compressor::Compress for each job
   ↓
On completion: dialog closes (or shows error / per-archive summary)
```

### 4.3 Win10 vs Win11 menu coexistence

```
                         Win10                Win11
                         -----                -----
Modern (IExplorerCommand) registered          shown in primary menu
   from AppxManifest      but Win10 shell
                          doesn't read the
                          schema — invisible

Classic (IContextMenu)    shown in primary    NOT shown — DLL detects
   from AppxManifest      menu                Win11 at QueryContextMenu
                                              entry and returns 0 items
                                              (so it's also absent from
                                              "Show more options")
```

The Win11 OS gate inside the classic DLL is essential. Without it, Win11 users would see duplicate OpenZip entries in the legacy "Show more options" submenu.

## 5. `OpenZipCore` — Compressor

New file: `src/core/compressor.h` / `src/core/compressor.cpp`.

```cpp
namespace openzip {

class Compressor {
public:
    enum class Result {
        Success,
        Cancelled,
        IoError,           // disk full, permission denied, etc.
        SourceMissing,     // a source path went away mid-run
        OutputExists,      // conflict callback returned Skip/Cancel
        BadPassword,       // shouldn't happen during write, reserved
    };

    enum class Level { Store = 0, Fast = 1, Normal = 6, Max = 9 };

    enum class Encoding {
        Utf8,    // sets the General Purpose Bit 11 flag
        Cp949,   // legacy Korean tools; no flag set
    };

    struct Options {
        Level level = Level::Normal;
        std::wstring password;                     // empty = no encryption
        Encoding filename_encoding = Encoding::Utf8;
        // 0 = auto, 1 = serial; jobs with < 4 entries always run serial.
        int concurrency = 0;
    };

    class ProgressCallback {
    public:
        virtual ~ProgressCallback() = default;

        virtual void OnEntryStart(const std::wstring& src_relpath,
                                  size_t index, size_t total_entries) = 0;
        virtual void OnBytes(uint64_t done, uint64_t total) = 0;

        // Same enum as Extractor::ConflictAction (Overwrite/Skip/Rename/Cancel).
        // Rename → Compressor picks "<stem> (1).zip" etc.
        virtual Extractor::ConflictAction OnOutputExists(
            const std::filesystem::path& output_zip) = 0;

        virtual bool ShouldCancel() = 0;
        virtual void OnComplete(Result) = 0;
    };

    // Compress N source paths (files and/or folders, folders recurse) into a
    // single output_zip. Internally writes to "<output_zip>.partial" and
    // renames atomically on success.
    static Result Compress(const std::vector<std::filesystem::path>& sources,
                           const std::filesystem::path& output_zip,
                           ProgressCallback& cb,
                           const Options& opts = {});
};

}  // namespace openzip
```

### 5.1 Behaviors and invariants

- **Atomic output** — write to `<output_zip>.partial`, rename on success, delete on failure or cancel. No half-written `.zip` left behind.
- **Relative paths inside the archive** — entries are stored relative to each source's parent.
  - Single folder `MyDocs\` selected → archive entries `MyDocs/file1.txt`, `MyDocs/sub/file2.txt`, …
  - 5 sibling files selected (e.g., `a.txt`, `b.txt`, … inside `Documents\`) → 5 flat entries `a.txt`, `b.txt`, … (no enclosing folder).
  - This is independent of the *output archive name*, which is determined by the menu verb (see §3 #7 for the multi-selection naming rule).
- **Folder recursion** — symbolic links are NOT followed (matches `Extractor`'s symlink refusal on the read side).
- **Encryption** — when `password` is non-empty, every entry is encrypted with WinZip-style AES-256 (`MZ_AES_ENCRYPTION_MODE_256`). ZipCrypto is read-only for OpenZip (we extract it, we don't create it) — its weakness is well known.
- **Filename encoding** — UTF-8 sets GP bit 11; CP949 does not. Encoding is per-archive (not per-entry). Conversion uses `WideCharToMultiByte(CP_UTF8 | 949, …)`. CP949-unmappable characters trigger a one-time warning callback in v0.3 (planned in implementation plan, not detailed here).
- **Decompression-bomb guard does not apply** to writes (only to reads).
- **Output path validation** — same `path_validator` rules: no Windows reserved names in the output zip's name, long paths via `\\?\`.

## 6. `OpenZipApp` — UI (MFC retained)

### 6.1 Command-line additions

Existing extract command line is unchanged. New compress invocations:

```
OpenZipApp.exe --compress
               --output <zip-path>
               --mode {bundle|each|prompt}
               [--level {store|fast|normal|max}]
               [--password <pw>]
               [--encoding {utf8|cp949}]
               --item <path> [--item <path> …]
```

`--mode prompt` opens `CCompressOptionsDialog` first; the dialog's OK proceeds with bundle semantics using the chosen output path.

### 6.2 New dialogs

- **`CCompressOptionsDialog`** — only shown when `--mode prompt`
  - Output filename (edit; default name follows §3 #7 — single item → that item's stem; multiple items → parent folder name; suffix `.zip`)
  - Output location (edit + Browse button using `IFileDialog`)
  - Compression level radios: Store / Fast / **Normal (default)** / Max
  - Password (edit + confirm edit + "show" checkbox); empty = no encryption
  - OK / Cancel

- **`CCompressDialog`** — progress dialog. Same layout class as `CExtractDialog` (consider sharing a base `CProgressDialogBase` if the divergence stays small; spec leaves the choice to the implementation plan)
  - Current source label (ellipsized)
  - Overall progress bar
  - Queue indicator ("3 of 5 archives")
  - Cancel button

### 6.3 Queue integration

The existing single-instance + named-pipe queue (`SingleInstance.cpp`) handles both extract and compress jobs. The job descriptor is extended with a discriminated union (extract job vs compress job). The first instance owns the queue; subsequent instances forward their args via the pipe and exit.

The existing extract dialog and the new compress dialog never run simultaneously in the same instance — the queue processes one job at a time. The dialog title bar shows the current job type.

### 6.4 Dark mode (applies to all dialogs, existing + new)

New file: `src/app/dark_theme.{h,cpp}`. Pure helper module, no MFC dependency in the header (callable from `CDialogEx::OnInitDialog`, message map handlers, etc.).

```cpp
namespace openzip::dark_theme {

bool IsDarkModeActive();   // reads HKCU\…\AppsUseLightTheme; cached per process
void EnableForWindow(HWND);          // titlebar + child controls
HBRUSH OnCtlColor(HWND ctl, HDC hdc, UINT msg);  // returns dark brush + sets text/bg
LRESULT OnCustomDrawProgressBar(NMHDR*); // for CProgressCtrl NM_CUSTOMDRAW
void ApplyToButton(HWND btn);             // SetWindowTheme + subclass for owner-draw
void ApplyToCommonControls(HWND parent);  // walks children, applies per type

}  // namespace openzip::dark_theme
```

Per-dialog wiring (one block in each `CDialogEx` subclass):

```cpp
BOOL CExtractDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();
    if (openzip::dark_theme::IsDarkModeActive()) {
        openzip::dark_theme::EnableForWindow(GetSafeHwnd());
        openzip::dark_theme::ApplyToCommonControls(GetSafeHwnd());
    }
    // … existing code …
    return TRUE;
}

HBRUSH CExtractDialog::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
    if (openzip::dark_theme::IsDarkModeActive()) {
        if (HBRUSH b = openzip::dark_theme::OnCtlColor(
                pWnd->GetSafeHwnd(), pDC->GetSafeHdc(), nCtlColor))
            return b;
    }
    return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
}
```

Targeted controls and techniques:

| Control | Technique |
|---|---|
| Title bar | `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` (Win11 22H2+; no-op on older) |
| Dialog background | `WM_CTLCOLORDLG` → return cached dark brush |
| Static, Edit | `WM_CTLCOLOR{STATIC,EDIT}` → set text color, bg color, return brush |
| Button (push) | Subclass + owner-draw with custom border/fill |
| Button (check/radio) | `SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr)` |
| Combobox | `SetWindowTheme(hwnd, L"DarkMode_CFD", nullptr)` |
| Progress bar | `NM_CUSTOMDRAW` — fill manually with dark accent |
| ListView (queue) | `SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr)` + `LVS_EX_DOUBLEBUFFER` |

Light-mode fallback: every helper checks `IsDarkModeActive()` and no-ops when light is in effect. `WM_SETTINGCHANGE` invalidates the cached value so OS-level theme changes take effect on next dialog open (not mid-dialog — sufficient for v0.3).

Undocumented `uxtheme.dll` ordinals (`SetPreferredAppMode`, etc.) are gated by feature detection: `GetProcAddress` by ordinal, no-op on failure. Best-effort, never crash.

## 7. Shell extensions

### 7.1 `OpenZipShellExt.dll` (modern, existing) — additions

Existing `.zip` extract verbs (CLSIDs `…1A0C/1A0D/1A0E`) are unchanged.

New compress verbs:

| Verb | CLSID suffix | Applies to | Title (KO / EN) |
|---|---|---|---|
| OpenZipCompress (parent) | `…1B00` | `*`, `Directory` | OpenZip |
| └ `<name>.zip`으로 압축 | `…1B01` | — | `"<n>.zip"으로 압축` / `Compress to "<n>.zip"` |
| └ 각각 압축 | `…1B02` | — | `각각 압축` / `Compress each separately` |
| └ 압축 옵션… | `…1B03` | — | `압축 옵션…` / `Compress with options…` |

`GetState` rules for the parent compress verb:
- Hide if the selection is exactly one `.zip` file (avoids visual collision with the extract verb).
- Hide if the selection has zero items.
- Otherwise show.

`Invoke` for each child verb spawns `OpenZipApp.exe` with the appropriate `--mode` and item list.

### 7.2 `OpenZipShellExtClassic.dll` (classic, NEW) — full responsibility list

Lives in `src/shellext_classic/`. Win32 DLL, no MFC, static CRT.

```cpp
class ClassicShellExt : public IShellExtInit, public IContextMenu {
    // IShellExtInit
    HRESULT Initialize(LPCITEMIDLIST, IDataObject* dobj, HKEY) override;
    // → Extract paths from CF_HDROP into items_

    // IContextMenu
    HRESULT QueryContextMenu(HMENU menu, UINT idx, UINT idCmdFirst,
                             UINT idCmdLast, UINT flags) override;
    // → If IsWin11OrLater() return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0)
    //   Else build menu based on items_ contents:
    //     - All items are .zip → Extract Here / Extract to <name>\
    //     - All items are non-zip files/folders → Compress submenu (3 items)
    //     - Mixed → Compress submenu only (extract verb requires single .zip)
    //     - Single .zip → Extract verbs on top, Compress submenu below

    HRESULT InvokeCommand(CMINVOKECOMMANDINFO* ici) override;
    HRESULT GetCommandString(UINT_PTR idCmd, UINT type, UINT*, CHAR* name, UINT cchMax) override;

private:
    std::vector<std::wstring> items_;
};

bool IsWin11OrLater() {
    // RtlGetVersion via GetProcAddress; fall back to GetVersionEx.
    // Win11 = build >= 22000.
}
```

### 7.3 Shared invocation contract

Both DLLs spawn `OpenZipApp.exe` with the **same command-line schema**. There is no IPC between the DLLs. The classic and modern DLLs are independent implementations of the same UX contract.

The app exe path is resolved as `GetModuleFileNameW(g_module)` → strip filename → append `OpenZipApp.exe`. Both DLLs and the exe live in the same MSIX install location.

### 7.4 Logging

Both DLLs append diagnostic events to `%LOCALAPPDATA%\OpenZip\shellext.log` (existing pattern from `OpenZipShellExt`). The classic DLL adds a `[classic]` tag to disambiguate.

## 8. MSIX manifest

### 8.1 MinVersion drop

```xml
<Dependencies>
  <TargetDeviceFamily Name="Windows.Desktop"
                      MinVersion="10.0.18362.0"
                      MaxVersionTested="10.0.26100.0"/>
</Dependencies>
```

`10.0.18362.0` is Win10 1903 (May 2019), the first build with MSIX shell-extension support.

### 8.2 New COM CLSIDs

```xml
<com:Extension Category="windows.comServer">
  <com:ComServer>
    <!-- existing modern shell ext (kept as-is) -->
    <com:SurrogateServer DisplayName="OpenZip Shell Extension">
      <com:Class Id="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1A0C"
                 Path="OpenZipShellExt.dll" ThreadingModel="STA"/>
      <!-- new compress parent verb -->
      <com:Class Id="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1B00"
                 Path="OpenZipShellExt.dll" ThreadingModel="STA"/>
    </com:SurrogateServer>

    <!-- new classic shell ext: in-process to explorer, NOT surrogate -->
    <com:Class Id="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F2000"
               Path="OpenZipShellExtClassic.dll" ThreadingModel="Apartment"/>
  </com:ComServer>
</com:Extension>
```

### 8.3 Modern compress verbs

```xml
<desktop4:Extension Category="windows.fileExplorerContextMenus">
  <desktop4:FileExplorerContextMenus>
    <!-- existing extract on .zip -->
    <desktop5:ItemType Type=".zip">
      <desktop5:Verb Id="OpenZipExtract" Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1A0C"/>
      <desktop5:Verb Id="OpenZipCompress" Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1B00"/>
    </desktop5:ItemType>
    <!-- new: compress on any file or folder -->
    <desktop5:ItemType Type="*">
      <desktop5:Verb Id="OpenZipCompress" Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1B00"/>
    </desktop5:ItemType>
    <desktop5:ItemType Type="Directory">
      <desktop5:Verb Id="OpenZipCompress" Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1B00"/>
    </desktop5:ItemType>
  </desktop4:FileExplorerContextMenus>
</desktop4:Extension>
```

### 8.4 Classic context-menu handler

Element name and exact attributes need a fact-check pass against current MSIX schema docs (see §11 risk #1). Provisional shape:

```xml
<desktop:Extension Category="windows.fileExplorerClassicContextMenuHandler"
                   EntryPoint="Windows.FullTrustApplication">
  <desktop:FileExplorerClassicContextMenuHandlers>
    <desktop:ItemType Type="*">
      <desktop:Handler Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F2000"/>
    </desktop:ItemType>
    <desktop:ItemType Type="Directory">
      <desktop:Handler Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F2000"/>
    </desktop:ItemType>
    <desktop:ItemType Type=".zip">
      <desktop:Handler Clsid="A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F2000"/>
    </desktop:ItemType>
  </desktop:FileExplorerClassicContextMenuHandlers>
</desktop:Extension>
```

If the schema turns out to differ, the implementation plan fixes element names; the registration *intent* (point three ItemTypes at the classic CLSID) does not change.

## 9. Testing strategy

### 9.1 Compressor unit tests (new)

Add to `tests/`:

- Round-trip: compress N synthetic files → extract → byte-equal contents
- Encoding parity: compress with UTF-8 → list raw filenames → bit 11 flag set; compress with CP949 → bit 11 unset, raw bytes are EUC-KR
- Korean filename round trip: file `테스트.txt` → compress UTF-8 → extract → name preserved
- Korean filename legacy compat: compress with CP949 → extract with old Bandizip on Win7 VM (manual; not CI)
- Atomic output: kill process mid-compress → only `.partial` exists, no `.zip`
- Folder recursion: nested 3-level folder with 100 files → all entries present at expected paths
- Symlink in source: skipped, not followed
- Output to read-only dir: returns `IoError`
- 1×4 GB synthetic file: Zip64 entry (already-existing `Extractor` round-trip test extended)

### 9.2 Shell ext smoke (manual, per release)

Win11 24H2 VM:
- Right-click `.zip` → modern menu: Extract verbs visible, Compress hidden
- Right-click random folder → modern menu: Compress visible (3 sub-verbs)
- Shift+F10 "Show more options" → no OpenZip entries (classic DLL OS-gate works)

Win10 22H2 VM:
- Right-click `.zip` → classic menu: Extract verbs at top, Compress submenu below
- Right-click folder → classic menu: Compress submenu only
- Right-click 5 files (mixed types) → classic menu: Compress submenu, "Compress to <parent>.zip"

Win10 1903 VM:
- Same as 22H2; this is the floor build for MSIX shell extensions

### 9.3 Dark mode visual review

Light + dark theme on Win11 24H2:
- Open each of: extract progress, password prompt, conflict dialog, compress options, compress progress
- Verify no flash of light during dialog open
- Verify focus indicators visible on dark
- Verify disabled controls readable on dark

Light only on Win10:
- All dialogs render in classic light theme; no broken brushes

### 9.4 Regression for existing v0.2 features

Run the v0.2 acceptance test set unchanged (Definition of Done items 1–11 in `plan.md` §11). v0.3 must not regress any.

## 10. Phasing

Implementation plan will detail tasks; phases listed here for design-time sanity:

1. `Compressor` engine + CLI test harness
2. Compress dialogs (options + progress) in MFC, queue integration
3. Modern shell ext compress verbs
4. Classic shell ext (Win10) + Win11 OS gate
5. MSIX manifest changes + sideload validation on Win10 1903 / 22H2 VMs
6. Dark mode helper + apply to all dialogs (existing 3 + new 2)
7. Documentation, README updates, screenshot refresh
8. Microsoft Store resubmission

## 11. Risks

In rough order of probability × impact:

1. **MSIX classic context-menu schema** — `windows.fileExplorerClassicContextMenuHandler` is a relatively recent MSIX capability. Element names and required attributes need to be verified against current Microsoft docs before manifest commit. Expect 1–2 hours of trial and error on first installation.

2. **Win10 sideload trust** — Microsoft Store distribution is unaffected, but sideloading the unsigned MSIX during dev/test on Win10 may require enabling Developer Mode and accepting a self-signed cert chain. Process is documented for VR; OpenZip can reuse.

3. **Dark mode undocumented APIs** — `SetPreferredAppMode`, `AllowDarkModeForWindow`, etc. are unexported `uxtheme.dll` symbols accessed by ordinal. They have stayed stable since Win10 1809 but a future Windows update could shift them. Mitigation: feature-detect via `GetProcAddress`; fall back to light mode silently if any symbol is missing.

4. **Classic shell ext crash blast radius** — classic DLLs load in-process to `explorer.exe`. A crash in `OpenZipShellExtClassic` crashes Explorer. Mitigations: keep the DLL minimal (no compression logic, no MFC, no third-party libs); wrap entry points in `try/catch (...)`; CI sanitizer build (ASan-equivalent: `/RTC1` + `/sdl`).

5. **Microsoft Store cert review** — adding a classic shell extension may trigger additional review beyond v0.2. Have justification text prepared ("required for Win10 file-context-menu integration of the same operations Win11 already supports").

6. **Compress menu name collision with built-in "Send to → Compressed (zipped) folder"** — Win11's native context menu also shows "Compress to ZIP file" (24H2+). Acceptable; OpenZip's entry is distinguished by the "OpenZip" parent label.

7. **Dialog sharing between extract and compress** — if `CExtractDialog` and `CCompressDialog` diverge enough that a shared base class hurts more than it helps, keep them separate. Decision deferred to implementation.

8. **CP949 encoding for non-Korean characters** — CP949 cannot encode characters outside its repertoire (Cyrillic, Arabic, …). When a source filename has unmappable characters and user chose CP949, fall back to UTF-8 for that archive and warn once. Implementation detail; not a design risk.

## 12. Definition of Done for v0.3.0

- [ ] Right-click any file/folder on Win11 24H2 → "OpenZip → 압축…" sub-verbs appear in modern menu
- [ ] Right-click `.zip` on Win11 24H2 still shows "OpenZip → 풀기" verbs (no regression)
- [ ] Right-click any file/folder on Win10 22H2 → "OpenZip" submenu appears in classic menu
- [ ] Right-click `.zip` on Win10 22H2 → both Extract and Compress submenu present
- [ ] Win11: classic OpenZip entries do NOT appear in "Show more options" submenu
- [ ] "Compress to `<name>.zip`" creates a valid archive readable by 7-Zip and Win11 native unzipper
- [ ] "Compress each separately" produces N archives in the same parent dir
- [ ] "Compress with options…" opens dialog; password ⇒ AES-256 encrypted archive verified by 7-Zip
- [ ] Korean filenames: UTF-8 default round-trips through 7-Zip and Win11 native; CP949 option round-trips through legacy Bandizip
- [ ] All dialogs render correctly in Win11 dark mode (no light flashes, all controls readable)
- [ ] All dialogs render correctly in Win10 light mode (no broken brushes from dark code paths)
- [ ] Cancel during compression deletes `.partial`, leaves no `.zip`
- [ ] All v0.2 acceptance tests still pass
- [ ] MSIX builds clean on CI; sideload-installs cleanly on Win10 1903, Win10 22H2, Win11 24H2
- [ ] Submitted to Microsoft Store as v0.3.0
