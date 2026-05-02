# OpenZip

A minimal, fast ZIP extractor for Windows 11 with native Explorer context-menu integration.

> 한국어 ZIP 파일 (CP949)을 깨짐 없이 풀어주는 가벼운 Windows 11 압축 해제기.

## Features

- **Modern context menu** — "Extract Here" / "Extract to <foldername>\" appear in Win11's modern right-click menu (not the legacy "Show more options" submenu).
- **Correct Korean filenames** — Robust CP949 / UTF-8 detection, no mojibake.
- **Password support** — Both ZipCrypto and AES-256 encrypted archives.
- **Fast** — minizip-ng + zlib-ng under the hood, single-instance queue prevents disk thrash.
- **Safe** — Zip-slip, symlink, decompression-bomb, and Windows reserved-name protections built in.
- **Light** — ZIP only; no compression, no preview UI, no telemetry, no auto-update.

## Install

Microsoft Store: https://www.microsoft.com/store/apps/9P09P5W5DPK0 *(pending review)*

## Screenshots

### Modern context menu

Right-click any `.zip` in File Explorer → **OpenZip** appears in the modern menu (no "Show more options" needed):

![Context menu](docs/screenshots/context-menu.png)

### Progress dialog (Korean UI on Korean systems)

The dialog auto-localizes when the user's MUI or regional locale is Korean:

![Progress](docs/screenshots/progress.png)

### Password prompt (ZipCrypto / AES-256)

Encrypted archives prompt for the password before the first encrypted entry:

![Password](docs/screenshots/password.png)

### Conflict resolution

If the destination already has a file, choose Overwrite / Skip / Rename / Cancel — with optional "apply to all":

![Conflict](docs/screenshots/conflict.png)

## Build from source

Prerequisites:
- Visual Studio 2022 with MFC and C++/CLI components
- Windows 10/11 SDK (latest)
- vcpkg integrated with VS (manifest mode)
- Python 3.10+ (for MSIX packaging script)

```powershell
git clone https://github.com/vitaldb/openzip.git
cd openzip
msbuild OpenZip.sln /p:Configuration=Release /p:Platform=x64
python msix\build_msix.py 0.1.0
# Sideload (Developer Mode required):
Add-AppxPackage -Register .\msix\staging\AppxManifest.xml
```

## Architecture

| Component | Type | Purpose |
|---|---|---|
| `OpenZipCore` | static lib | Extraction engine, filename decoding, path validation |
| `OpenZipApp` | MFC EXE | Progress UI, password prompt, conflict dialog |
| `OpenZipShellExt` | Win32 DLL | `IExplorerCommand` for Win11 modern context menu |

See [`plan.md`](./plan.md) for the full design rationale.

## License

MIT — see [LICENSE](./LICENSE).

Third-party libraries:
- [minizip-ng](https://github.com/zlib-ng/minizip-ng) — zlib license
- [zlib-ng](https://github.com/zlib-ng/zlib-ng) — zlib license

## Credits

Developed by the [VitalDB](https://vitaldb.net) lab at Seoul National University Hospital.
