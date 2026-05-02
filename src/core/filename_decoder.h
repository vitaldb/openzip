#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace openzip {

// Which encoding was used to decode the filename.
enum class DecodeSource {
    Utf8Flag,  // ZIP UTF-8 flag was set; decoded as UTF-8.
    Ascii,     // All bytes < 0x80; unambiguous.
    Utf8,      // Strict UTF-8 succeeded (no flag, but bytes form valid UTF-8).
    Cp949,     // Korean codepage 949 decode; preferred when Hangul codepoints appear.
    Cp437,     // Last-resort OEM/DOS fallback.
};

struct DecodedName {
    std::wstring text;
    DecodeSource source;
};

// Decode a raw ZIP filename byte sequence to wide string per the strategy in plan.md §5.
//
// Order:
//   1. utf8_flag set + strict UTF-8 succeeds  → Utf8Flag
//   2. all bytes < 0x80                       → Ascii
//   3. strict UTF-8 succeeds                  → Utf8 (unless CP949 yields Hangul and UTF-8 doesn't)
//   4. strict CP949 succeeds                  → Cp949
//   5. CP437 (single-byte, never fails)       → Cp437
DecodedName DecodeFilename(const uint8_t* bytes, size_t len, bool utf8_flag);

inline DecodedName DecodeFilename(const std::vector<uint8_t>& bytes, bool utf8_flag) {
    return DecodeFilename(bytes.data(), bytes.size(), utf8_flag);
}

inline DecodedName DecodeFilename(const char* bytes, size_t len, bool utf8_flag) {
    return DecodeFilename(reinterpret_cast<const uint8_t*>(bytes), len, utf8_flag);
}

}  // namespace openzip
