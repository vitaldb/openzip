#pragma once

#include "core/secure_string.h"

#include <filesystem>
#include <string>
#include <vector>

namespace openzip {

enum class JobKind    { Extract, Compress };
enum class CompressMode { Bundle, Each, Prompt };

// Parsed `openzip.exe` command line.
//
// Extract forms:
//   openzip.exe --extract <zip> [--target <dir>] [--here|--folder] [--password <pw>]
//   openzip.exe <zip>                          (file-association double-click)
//   openzip.exe --help
//
// `--here`  → target = zip's parent directory (Bandizip "Extract Here").
// `--folder`→ target = zip's parent / zip stem (Bandizip "Extract to <name>\").
// neither   → defaults to `--folder` semantics (safer for double-click).
//
// Compress forms:
//   openzip.exe --compress --output <zip> [--mode bundle|each|prompt]
//                             [--level store|fast|normal|max]
//                             [--encoding utf8|cp949]
//                             --item <path> [--item <path>...]
struct CommandLine {
    JobKind kind = JobKind::Extract;

    // ── Extract-mode fields ────────────────────────────────────────
    std::filesystem::path zip_path;
    std::filesystem::path target_dir;   // resolved against zip_path + flags
    std::wstring password;              // empty = prompt
    int threads = 0;                    // 0 = auto, 1 = legacy serial, N = N threads

    // ── Compress-mode fields ───────────────────────────────────────
    std::vector<std::filesystem::path> compress_items;
    std::filesystem::path compress_output;
    CompressMode compress_mode  = CompressMode::Bundle;
    int compress_level          = 6;       // matches Level::Normal
    std::wstring compress_encoding = L"utf8";

    bool show_help    = false;
    bool show_browser = false;  // true when invoked with bare zip path (file-association double-click)
    bool valid        = true;
    std::wstring error;

    // Wipe the plaintext password when this struct dies. Each std::wstring
    // copy lives in its own buffer, so every copy must reach destruction
    // to be scrubbed — treat as defense-in-depth.
    ~CommandLine() { SecureZero(password); }
    CommandLine() = default;
    CommandLine(const CommandLine&) = default;
    CommandLine(CommandLine&&) = default;
    CommandLine& operator=(const CommandLine&) = default;
    CommandLine& operator=(CommandLine&&) = default;
};

// Parse a Win32 command-line string (typically GetCommandLineW()) and resolve target_dir.
CommandLine ParseCommandLine(const wchar_t* cmdline);

}  // namespace openzip
