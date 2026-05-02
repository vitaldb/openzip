# Test Archives

Sample ZIP files for verifying `OpenZipCore` against the 9 cases listed in `plan.md` Phase 1.

Place each archive here using the filename in the table below; the integration test runner
will pick them up by name.

| File | Case | How to construct |
|---|---|---|
| `utf8_basic.zip` | Modern UTF-8 zip | `Compress-Archive` (PowerShell) on a folder containing Korean filenames. The general purpose flag bit 11 (UTF-8) will be set. |
| `cp949_legacy.zip` | Legacy CP949 zip | Use 7-Zip CLI on a Korean Windows: `7z a -mcu=off cp949_legacy.zip 한글파일.txt` — `-mcu=off` disables UTF-8 flag, forcing CP949. Or copy from a real archive received from a Korean Windows user. |
| `pkcrypt.zip` | Password-protected (ZipCrypto / PKZIP encryption) | `7z a -p"password" -mem=ZipCrypto pkcrypt.zip *` (older "Standard ZIP 2.0 encryption") |
| `aes256.zip` | Password-protected (AES-256, WinZip-style) | `7z a -p"password" -mem=AES256 aes256.zip *` |
| `traversal.zip` | Directory traversal attempt | Hand-craft an entry with `../../../etc/passwd` or `..\\..\\..\\Windows\\System32\\evil.dll`. Tools like `evilarc` or just `python -c "import zipfile; z=zipfile.ZipFile('traversal.zip','w'); z.writestr('../../etc/passwd', 'pwn'); z.close()"` work. |
| `reserved.zip` | Windows reserved names | A zip with entries named `CON.txt`, `PRN.dat`, `COM1`, etc. Construct with `python -c "import zipfile; z=zipfile.ZipFile('reserved.zip','w'); z.writestr('CON.txt', 'oops'); z.close()"` |
| `empty.zip` | Empty archive (no entries) | `python -c "import zipfile; zipfile.ZipFile('empty.zip','w').close()"` |
| `many_files.zip` | 10,000+ small files | A zip containing 10,000 single-byte text files. `python -c "import zipfile; z=zipfile.ZipFile('many_files.zip','w'); [z.writestr(f'f/{i:05d}.txt', '.') for i in range(10000)]; z.close()"` |
| `zip64_big.zip` | Single file > 4GB (Zip64) | Generate a 5GB random file and zip it. Skip in CI; run manually. `python -c "with open('big.bin','wb') as f: f.write(b'x'*5*1024*1024*1024)"` then zip it. |

## Expected results

| File | Expected Result enum |
|---|---|
| `utf8_basic.zip` | `Success`; entries decoded with `Utf8Flag` source |
| `cp949_legacy.zip` | `Success`; entries decoded with `Cp949` source; no mojibake |
| `pkcrypt.zip` | Without password: prompts. With correct: `Success`. With wrong: `BadPassword` |
| `aes256.zip` | Same as `pkcrypt.zip` but with AES |
| `traversal.zip` | `UnsafePath` |
| `reserved.zip` | `ReservedName` |
| `empty.zip` | `Success` (no-op) |
| `many_files.zip` | `Success`; reasonable extraction time |
| `zip64_big.zip` | `Success` (or `BombRefused` if ratio triggers — verify it doesn't) |

## Running the harness

```powershell
# List entries and decode source for all archives
$env:OPENZIP_LIST = '1'
foreach ($z in Get-ChildItem tests\test_archives\*.zip) {
    Write-Host "=== $($z.Name) ==="
    & .\x64\Debug\CliTest.exe $z.FullName
}
Remove-Item Env:OPENZIP_LIST

# Extract one archive
& .\x64\Debug\CliTest.exe tests\test_archives\utf8_basic.zip $env:TEMP\out

# Pre-supply password for encrypted ones
$env:OPENZIP_PASSWORD = 'password'
& .\x64\Debug\CliTest.exe tests\test_archives\aes256.zip $env:TEMP\out
Remove-Item Env:OPENZIP_PASSWORD
```

## .gitignore note

These archive files are ignored by default (we don't want to commit binary test data
to the repo). When you collect them, leave them locally. CI can fetch them from a
release tag if we choose to publish a separate test-data release later.
