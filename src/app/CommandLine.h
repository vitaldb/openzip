#pragma once

#include <filesystem>
#include <string>

namespace openzip {

// Parsed `OpenZipApp.exe` command line.
//
// Forms:
//   OpenZipApp.exe --extract <zip> [--target <dir>] [--here|--folder] [--password <pw>]
//   OpenZipApp.exe <zip>                          (file-association double-click)
//   OpenZipApp.exe --help
//
// `--here`  → target = zip's parent directory (Bandizip "Extract Here").
// `--folder`→ target = zip's parent / zip stem (Bandizip "Extract to <name>\").
// neither   → defaults to `--folder` semantics (safer for double-click).
struct CommandLine {
    std::filesystem::path zip_path;
    std::filesystem::path target_dir;     // resolved against zip_path + flags
    std::wstring password;                // empty = prompt
    bool show_help = false;
    bool valid = true;
    std::wstring error;
};

// Parse a Win32 command-line string (typically GetCommandLineW()) and resolve target_dir.
CommandLine ParseCommandLine(const wchar_t* cmdline);

}  // namespace openzip
