#pragma once

#include <filesystem>
#include <string>

namespace openzip {

enum class PathError {
    Ok,
    EmptyPath,
    AbsolutePath,    // entry contains drive letter, UNC, or leading slash
    EscapesTarget,   // normalized path escapes the target dir (zip-slip)
    ReservedName,    // contains a Windows reserved name (CON, PRN, etc.) or trailing dots/spaces
    InvalidChar,     // contains characters NTFS bans or treats specially
                     // (':' creates an Alternate Data Stream; '<>"|?*' or
                     // control characters 0x00-0x1F are NTFS-illegal)
};

struct ValidatedPath {
    PathError error = PathError::Ok;
    std::filesystem::path absolute;  // absolute, lexically normalized; no \\?\ prefix
};

// Validate and resolve a ZIP entry filename against a target directory.
//
// Sanitization performed:
//   - forward slashes converted to backslashes
//   - leading slashes stripped
//   - components checked for ., .., reserved names, trailing dots/spaces
//   - resulting absolute path verified to remain under target_dir after normalization
ValidatedPath ValidatePath(const std::wstring& zip_entry_name,
                           const std::filesystem::path& target_dir);

// True if the given path component is a Windows reserved device name
// (CON, PRN, AUX, NUL, COM1-9, LPT1-9, CLOCK$). Comparison strips the
// extension portion (so "CON.txt" returns true) and is case-insensitive.
bool IsReservedName(const std::wstring& component);

// Return a path string suitable for Win32 wide-string APIs that support
// long paths. Adds the \\?\ prefix when needed. Use this when calling
// CreateFileW, CreateDirectoryW, etc. with paths near or beyond MAX_PATH.
std::wstring MakeLongPath(const std::filesystem::path& p);

}  // namespace openzip
