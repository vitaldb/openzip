#include "path_validator.h"

#include <algorithm>
#include <vector>

namespace fs = std::filesystem;

namespace openzip {

namespace {

constexpr size_t kMaxPathClassic = 260;

bool IEquals(const std::wstring& a, const wchar_t* b) {
    size_t i = 0;
    for (; b[i] != 0 && i < a.size(); ++i) {
        wchar_t ac = a[i];
        wchar_t bc = b[i];
        if (ac >= L'a' && ac <= L'z') ac = static_cast<wchar_t>(ac - L'a' + L'A');
        if (bc >= L'a' && bc <= L'z') bc = static_cast<wchar_t>(bc - L'a' + L'A');
        if (ac != bc) return false;
    }
    return b[i] == 0 && i == a.size();
}

void RTrimDotsAndSpaces(std::wstring& s) {
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) {
        s.pop_back();
    }
}

std::vector<std::wstring> SplitOnBackslash(const std::wstring& s) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L'\\') {
            if (!cur.empty()) {
                parts.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(std::move(cur));
    return parts;
}

}  // namespace

// True if the component contains a character that NTFS forbids in filenames
// (':', '<', '>', '"', '|', '?', '*') or any C0 control character (0x00-0x1F).
//
// We single out ':' because on NTFS it does not raise an error — it silently
// creates or addresses an Alternate Data Stream attached to a sibling file.
// A ZIP entry named `legitimate.txt:malicious.exe` could land inside an
// existing `legitimate.txt` as a hidden ADS, which most users have no way
// to inspect. The other characters are rejected for consistency: NTFS would
// fail the create with ERROR_INVALID_NAME, but reporting it as a security
// refusal is clearer than a generic IO error and avoids leaving partial
// state behind from earlier entries that did succeed.
bool ComponentHasInvalidChar(const std::wstring& component) {
    for (wchar_t c : component) {
        if (c < 0x20) return true;
        switch (c) {
            case L':':
            case L'<':
            case L'>':
            case L'"':
            case L'|':
            case L'?':
            case L'*':
                return true;
            default:
                break;
        }
    }
    return false;
}

bool IsReservedName(const std::wstring& component) {
    // Strip everything from the first dot. "CON.txt" → "CON" is reserved.
    std::wstring base = component;
    auto dot = base.find(L'.');
    if (dot != std::wstring::npos) base.resize(dot);

    // After stripping, also strip trailing spaces (kernel-level reservation includes them).
    RTrimDotsAndSpaces(base);

    static const wchar_t* const kReserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL", L"CLOCK$",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9",
    };
    for (const wchar_t* r : kReserved) {
        if (IEquals(base, r)) return true;
    }
    return false;
}

ValidatedPath ValidatePath(const std::wstring& zip_entry_name, const fs::path& target_dir) {
    ValidatedPath result;

    if (zip_entry_name.empty()) {
        result.error = PathError::EmptyPath;
        return result;
    }

    // Convert forward slashes to backslashes.
    std::wstring norm = zip_entry_name;
    std::replace(norm.begin(), norm.end(), L'/', L'\\');

    // Reject drive letter (X:...).
    if (norm.size() >= 2 && ((norm[0] >= L'A' && norm[0] <= L'Z') || (norm[0] >= L'a' && norm[0] <= L'z'))
        && norm[1] == L':') {
        result.error = PathError::AbsolutePath;
        return result;
    }
    // Reject UNC (\\server\share).
    if (norm.size() >= 2 && norm[0] == L'\\' && norm[1] == L'\\') {
        result.error = PathError::AbsolutePath;
        return result;
    }
    // Strip leading backslashes (treat /foo as foo).
    size_t lead = 0;
    while (lead < norm.size() && norm[lead] == L'\\') ++lead;
    if (lead > 0) norm.erase(0, lead);

    if (norm.empty()) {
        result.error = PathError::EmptyPath;
        return result;
    }

    // Component-level checks.
    auto components = SplitOnBackslash(norm);
    int depth = 0;
    for (const auto& comp : components) {
        if (comp == L".") continue;
        if (comp == L"..") {
            if (--depth < 0) {
                result.error = PathError::EscapesTarget;
                return result;
            }
            continue;
        }
        if (ComponentHasInvalidChar(comp)) {
            result.error = PathError::InvalidChar;
            return result;
        }
        if (IsReservedName(comp)) {
            result.error = PathError::ReservedName;
            return result;
        }
        // Trailing dots/spaces are silently dropped by the OS — refuse to create such names.
        std::wstring trimmed = comp;
        RTrimDotsAndSpaces(trimmed);
        if (trimmed.empty()) {
            result.error = PathError::ReservedName;
            return result;
        }
        ++depth;
    }

    // Build absolute path under target and verify containment after normalization.
    fs::path target_norm = fs::absolute(target_dir).lexically_normal();
    fs::path abs = (target_norm / fs::path(norm)).lexically_normal();

    std::wstring abs_str = abs.wstring();
    std::wstring target_str = target_norm.wstring();
    if (!target_str.empty() && target_str.back() != L'\\') target_str.push_back(L'\\');

    bool inside =
        abs_str.size() >= target_str.size() - 1 &&
        (abs_str == target_str.substr(0, target_str.size() - 1) ||  // exactly target
         (abs_str.size() >= target_str.size() &&
          abs_str.compare(0, target_str.size(), target_str) == 0));
    if (!inside) {
        result.error = PathError::EscapesTarget;
        return result;
    }

    result.error = PathError::Ok;
    result.absolute = std::move(abs);
    return result;
}

std::wstring MakeLongPath(const fs::path& p) {
    std::wstring s = p.wstring();
    if (s.size() < kMaxPathClassic) return s;
    if (s.size() >= 4 && s.compare(0, 4, L"\\\\?\\") == 0) return s;
    if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\') {
        // UNC \\server\share → \\?\UNC\server\share
        return L"\\\\?\\UNC\\" + s.substr(2);
    }
    return L"\\\\?\\" + s;
}

}  // namespace openzip
