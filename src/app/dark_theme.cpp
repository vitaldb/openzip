#include "stdafx.h"
#include "dark_theme.h"

#include <dwmapi.h>
#include <Uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace openzip::dark_theme {

namespace {
constexpr COLORREF kBgDark   = RGB(32, 32, 32);
constexpr COLORREF kFgDark   = RGB(220, 220, 220);
constexpr COLORREF kEditDark = RGB(45, 45, 45);

bool g_cached_valid = false;
bool g_cached = false;
HBRUSH g_brush_bg = nullptr;
HBRUSH g_brush_edit = nullptr;

bool ReadAppsUseLightTheme() {
    HKEY k;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &k) != ERROR_SUCCESS) return true;
    DWORD val = 1, sz = sizeof(val), type = 0;
    LSTATUS s = ::RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, &type,
                                   reinterpret_cast<LPBYTE>(&val), &sz);
    ::RegCloseKey(k);
    if (s != ERROR_SUCCESS || type != REG_DWORD) return true;
    return val != 0;
}

void EnsureBrushes() {
    if (!g_brush_bg)   g_brush_bg   = ::CreateSolidBrush(kBgDark);
    if (!g_brush_edit) g_brush_edit = ::CreateSolidBrush(kEditDark);
}

}  // namespace

bool IsDarkModeActive() {
    if (!g_cached_valid) {
        g_cached = !ReadAppsUseLightTheme();
        g_cached_valid = true;
    }
    return g_cached;
}

void Reset() { g_cached_valid = false; }

void EnableForWindow(HWND hwnd) {
    if (!IsDarkModeActive()) return;
    EnsureBrushes();

    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                            &dark, sizeof(dark));

    ::EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
        wchar_t cls[64]{};
        ::GetClassNameW(child, cls, 64);
        if (lstrcmpiW(cls, L"Button") == 0 || lstrcmpiW(cls, L"ComboBox") == 0)
            ::SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
        else if (lstrcmpiW(cls, L"Edit") == 0)
            ::SetWindowTheme(child, L"DarkMode_CFD", nullptr);
        else if (lstrcmpiW(cls, L"SysListView32") == 0)
            ::SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
        return TRUE;
    }, 0);
}

HBRUSH OnCtlColor(HWND, HDC hdc, UINT nMsg) {
    if (!IsDarkModeActive()) return nullptr;
    EnsureBrushes();
    ::SetTextColor(hdc, kFgDark);
    if (nMsg == WM_CTLCOLOREDIT) {
        ::SetBkColor(hdc, kEditDark);
        return g_brush_edit;
    }
    ::SetBkColor(hdc, kBgDark);
    return g_brush_bg;
}

LRESULT OnProgressCustomDraw(NMHDR* hdr) {
    auto* nmcd = reinterpret_cast<LPNMCUSTOMDRAW>(hdr);
    if (nmcd->dwDrawStage == CDDS_PREERASE) {
        if (!IsDarkModeActive()) return CDRF_DODEFAULT;
        EnsureBrushes();
        ::FillRect(nmcd->hdc, &nmcd->rc, g_brush_bg);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

}  // namespace openzip::dark_theme
