#include "filename_decoder.h"

#include <Windows.h>

namespace openzip {

namespace {

bool IsAllAscii(const uint8_t* bytes, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (bytes[i] >= 0x80) return false;
    }
    return true;
}

bool TryDecode(UINT codepage, DWORD flags, const uint8_t* bytes, size_t len, std::wstring& out) {
    if (len == 0) {
        out.clear();
        return true;
    }
    int wlen = MultiByteToWideChar(codepage, flags,
                                   reinterpret_cast<const char*>(bytes), static_cast<int>(len),
                                   nullptr, 0);
    if (wlen <= 0) return false;
    out.resize(static_cast<size_t>(wlen));
    int written = MultiByteToWideChar(codepage, flags,
                                      reinterpret_cast<const char*>(bytes), static_cast<int>(len),
                                      out.data(), wlen);
    return written > 0;
}

bool HasHangul(const std::wstring& s) {
    for (wchar_t c : s) {
        if ((c >= 0xAC00 && c <= 0xD7AF) ||  // Syllables
            (c >= 0x1100 && c <= 0x11FF) ||  // Jamo
            (c >= 0x3130 && c <= 0x318F)) {  // Compatibility Jamo
            return true;
        }
    }
    return false;
}

}  // namespace

DecodedName DecodeFilename(const uint8_t* bytes, size_t len, bool utf8_flag) {
    DecodedName result;

    // 1. UTF-8 flag set → trust as UTF-8.
    //    Some buggy tools set the flag but write CP949 anyway, so fall through on decode failure.
    if (utf8_flag) {
        if (TryDecode(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, len, result.text)) {
            result.source = DecodeSource::Utf8Flag;
            return result;
        }
    }

    // 2. Pure ASCII — unambiguous.
    if (IsAllAscii(bytes, len)) {
        result.text.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            result.text.push_back(static_cast<wchar_t>(bytes[i]));
        }
        result.source = DecodeSource::Ascii;
        return result;
    }

    // 3. Strict UTF-8 decode. CP949 byte sequences rarely produce valid UTF-8 by accident;
    //    when both succeed, prefer CP949 if it yields Hangul and UTF-8 doesn't.
    std::wstring utf8_attempt;
    if (TryDecode(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, len, utf8_attempt)) {
        std::wstring cp949_attempt;
        if (TryDecode(949, MB_ERR_INVALID_CHARS, bytes, len, cp949_attempt) &&
            HasHangul(cp949_attempt) && !HasHangul(utf8_attempt)) {
            result.text = std::move(cp949_attempt);
            result.source = DecodeSource::Cp949;
            return result;
        }
        result.text = std::move(utf8_attempt);
        result.source = DecodeSource::Utf8;
        return result;
    }

    // 4. CP949 strict decode.
    std::wstring cp949_attempt;
    if (TryDecode(949, MB_ERR_INVALID_CHARS, bytes, len, cp949_attempt)) {
        result.text = std::move(cp949_attempt);
        result.source = DecodeSource::Cp949;
        return result;
    }

    // 5. CP437 fallback (single-byte; never fails).
    TryDecode(437, 0, bytes, len, result.text);
    result.source = DecodeSource::Cp437;
    return result;
}

}  // namespace openzip
