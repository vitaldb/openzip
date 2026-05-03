#pragma once

#include <Windows.h>

#include <string>

namespace openzip {

// Overwrite the in-memory bytes of a string with zero before clearing it.
// Use for password material so plaintext does not linger in process memory
// after the cleartext is no longer needed (helpful against crash dumps,
// page-file leakage, and post-mortem forensics).
//
// Caveats: this only sweeps the *current* allocation. If the string was
// copied or its storage was relocated by an earlier reserve()/move, the
// old buffers still hold the secret. Treat it as defense-in-depth, not
// a guarantee — never base trust decisions on "the password is gone".
inline void SecureZero(std::wstring& s) noexcept {
    if (!s.empty()) {
        ::SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
    }
    s.clear();
}

inline void SecureZero(std::string& s) noexcept {
    if (!s.empty()) {
        ::SecureZeroMemory(s.data(), s.size());
    }
    s.clear();
}

// RAII wrapper: zeroes the bound string when the guard goes out of scope.
// Use to protect locals that may take an early return path:
//
//     std::wstring pw = cb.OnPasswordRequired(...);
//     SecureZeroOnExit guard(pw);
//     ... use pw ...
//     // pw is zeroed here even on exception or early return.
template <typename String>
class SecureZeroOnExit {
public:
    explicit SecureZeroOnExit(String& s) noexcept : s_(s) {}
    ~SecureZeroOnExit() noexcept { SecureZero(s_); }
    SecureZeroOnExit(const SecureZeroOnExit&) = delete;
    SecureZeroOnExit& operator=(const SecureZeroOnExit&) = delete;
private:
    String& s_;
};

}  // namespace openzip
