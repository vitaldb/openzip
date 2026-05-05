#include "stdafx.h"
#include "dark_theme.h"

#include <CommCtrl.h>
#include <dwmapi.h>
#include <Uxtheme.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace openzip::dark_theme {

namespace {
constexpr COLORREF kBgDark       = RGB(32, 32, 32);
constexpr COLORREF kFgDark       = RGB(220, 220, 220);
constexpr COLORREF kEditDark     = RGB(45, 45, 45);
constexpr COLORREF kHeaderBgDark = RGB(43, 43, 43);  // matches Win11 Explorer's header
constexpr COLORREF kHeaderSepDark = RGB(60, 60, 60);

// Light-mode counterparts. Owner-drawing in light mode keeps the rendering
// path identical to dark mode (single code path → easier to keep them in
// sync) and matches what flat_button does for push buttons.
//
// The header bg matches the listview body (both white) so the whole
// listview reads as one bright panel sitting on the gray dialog backdrop
// (system COLOR_3DFACE ~ RGB(240,240,240)). The panel-vs-backdrop
// separation comes from CArchiveBrowserDialog's outline draw, not from
// a header color tier (which we tried before — it just made the header
// look like a continuation of the dialog).
constexpr COLORREF kFgLight         = RGB(28, 28, 28);
constexpr COLORREF kHeaderBgLight   = RGB(255, 255, 255);
constexpr COLORREF kHeaderSepLight  = RGB(213, 213, 213);
constexpr COLORREF kHeaderUnderLight = RGB(213, 213, 213);  // bottom border line

constexpr UINT_PTR kHeaderSubId = 0x0DA47000UL;

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

// Subclass procedure for the listview's column-header child (SysHeader32).
//
// Paints in BOTH dark and light modes so the rendering path is the same
// regardless of theme — same code, just a different palette per call.
// Without this, light mode used to fall back to the default themed header
// while dark mode was owner-drawn, leaving the look inconsistent with the
// flat_button push buttons (which already paint themselves in both modes).
LRESULT CALLBACK HeaderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                            UINT_PTR id, DWORD_PTR /*data*/) {
    switch (msg) {
        case WM_ERASEBKGND:
            // Suppress default erase — WM_PAINT fully repaints, and the
            // default themed erase would briefly flash the system bg color
            // before our paint runs.
            return 1;

        case WM_PAINT: {
            const bool     dark = IsDarkModeActive();
            const COLORREF bg   = dark ? kHeaderBgDark  : kHeaderBgLight;
            const COLORREF fg   = dark ? kFgDark        : kFgLight;
            const COLORREF sep  = dark ? kHeaderSepDark : kHeaderSepLight;

            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(hwnd, &ps);
            if (!dc) return 0;

            RECT client;
            ::GetClientRect(hwnd, &client);
            HBRUSH bg_br = ::CreateSolidBrush(bg);
            ::FillRect(dc, &client, bg_br);
            ::DeleteObject(bg_br);

            HFONT font = reinterpret_cast<HFONT>(::SendMessageW(hwnd, WM_GETFONT, 0, 0));
            HFONT old_font = font ? static_cast<HFONT>(::SelectObject(dc, font)) : nullptr;
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, fg);

            int count = static_cast<int>(::SendMessageW(hwnd, HDM_GETITEMCOUNT, 0, 0));
            for (int i = 0; i < count; ++i) {
                RECT ir{};
                if (!::SendMessageW(hwnd, HDM_GETITEMRECT, i,
                                    reinterpret_cast<LPARAM>(&ir))) continue;

                wchar_t text[256] = {};
                HDITEMW hdi{};
                hdi.mask       = HDI_TEXT | HDI_FORMAT;
                hdi.pszText    = text;
                hdi.cchTextMax = 256;
                ::SendMessageW(hwnd, HDM_GETITEMW, i,
                               reinterpret_cast<LPARAM>(&hdi));

                // Right-edge separator (inset top/bottom by 4 px, matching
                // Win11 Explorer's "short tick" between columns). Skip the
                // last column — its right edge meets the listview frame
                // and an extra tick reads as noise.
                if (i + 1 < count) {
                    HPEN pen     = ::CreatePen(PS_SOLID, 1, sep);
                    HPEN old_pen = static_cast<HPEN>(::SelectObject(dc, pen));
                    ::MoveToEx(dc, ir.right - 1, ir.top + 4, nullptr);
                    ::LineTo(dc, ir.right - 1, ir.bottom - 4);
                    ::SelectObject(dc, old_pen);
                    ::DeleteObject(pen);
                }

                // Lay out caption + (optional) sort triangle as a single
                // unit so the triangle sits immediately beside the
                // header text. The unit gets aligned per the column's
                // HDF_LEFT/CENTER/RIGHT format — for right-aligned
                // columns the triangle ends up at the right edge with
                // the text just left of it; for left-aligned columns
                // the text is at the left and the triangle sits to its
                // right. Without this the indicator would always sit
                // at the cell's right edge regardless of where the text
                // landed, which read as disconnected from the column.
                const bool show_sort = (hdi.fmt & (HDF_SORTUP | HDF_SORTDOWN)) != 0;
                constexpr int kTriHalf  = 4;   // triangle radius on x axis
                constexpr int kTriGap   = 6;   // text → triangle gap
                const int sort_extra = show_sort ? (kTriGap + 2 * kTriHalf) : 0;

                RECT tr = ir;
                tr.left  += 8;
                tr.right -= 8;
                int inner_w = tr.right - tr.left;

                UINT align = DT_LEFT;
                if (hdi.fmt & HDF_CENTER)     align = DT_CENTER;
                else if (hdi.fmt & HDF_RIGHT) align = DT_RIGHT;

                SIZE ts{};
                ::GetTextExtentPoint32W(dc, text, lstrlenW(text), &ts);
                int max_text_w = inner_w - sort_extra;
                if (max_text_w < 0) max_text_w = 0;
                int text_w = ts.cx > max_text_w ? max_text_w : ts.cx;
                int unit_w = text_w + sort_extra;

                int unit_left = tr.left;
                if (align == DT_RIGHT)       unit_left = tr.right - unit_w;
                else if (align == DT_CENTER) unit_left = (tr.left + tr.right - unit_w) / 2;
                if (unit_left < tr.left) unit_left = tr.left;

                RECT text_rect = { unit_left, ir.top,
                                   unit_left + text_w, ir.bottom };
                ::DrawTextW(dc, text, -1, &text_rect,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE
                                    | DT_END_ELLIPSIS | DT_NOPREFIX);

                if (show_sort) {
                    const bool down = (hdi.fmt & HDF_SORTDOWN) != 0;
                    const int  cx = unit_left + text_w + kTriGap + kTriHalf;
                    const int  cy = (ir.top + ir.bottom) / 2;
                    POINT tri[3];
                    if (down) {
                        tri[0] = { cx - kTriHalf, cy - 2 };
                        tri[1] = { cx + kTriHalf, cy - 2 };
                        tri[2] = { cx,            cy + 3 };
                    } else {
                        tri[0] = { cx,            cy - 3 };
                        tri[1] = { cx + kTriHalf, cy + 2 };
                        tri[2] = { cx - kTriHalf, cy + 2 };
                    }
                    HBRUSH tri_br  = ::CreateSolidBrush(fg);
                    HBRUSH old_br  = static_cast<HBRUSH>(::SelectObject(dc, tri_br));
                    HPEN   old_pen = static_cast<HPEN>(
                        ::SelectObject(dc, ::GetStockObject(NULL_PEN)));
                    ::Polygon(dc, tri, 3);
                    ::SelectObject(dc, old_br);
                    ::SelectObject(dc, old_pen);
                    ::DeleteObject(tri_br);
                }
            }

            // Bottom border (light mode only) — gives the header a clean
            // 1px line separating it from the file rows below, like
            // Win11 Explorer. In dark mode the body/header color tier
            // is enough on its own.
            if (!dark) {
                HPEN under   = ::CreatePen(PS_SOLID, 1, kHeaderUnderLight);
                HPEN old_pen = static_cast<HPEN>(::SelectObject(dc, under));
                ::MoveToEx(dc, client.left, client.bottom - 1, nullptr);
                ::LineTo(dc, client.right, client.bottom - 1);
                ::SelectObject(dc, old_pen);
                ::DeleteObject(under);
            }

            if (old_font) ::SelectObject(dc, old_font);
            ::EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCDESTROY:
            ::RemoveWindowSubclass(hwnd, HeaderProc, id);
            break;
    }
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

void SubclassHeader(HWND header) {
    if (!header) return;
    DWORD_PTR existing = 0;
    if (::GetWindowSubclass(header, HeaderProc, kHeaderSubId, &existing))
        return;  // already subclassed
    ::SetWindowSubclass(header, HeaderProc, kHeaderSubId, 0);
    ::InvalidateRect(header, nullptr, TRUE);
}

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
        else if (lstrcmpiW(cls, L"SysListView32") == 0) {
            ::SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
            // Listview's column header is a separate child window the
            // DarkMode_Explorer theme on the listview itself does not
            // reach. Theme it for the hover/pressed glyphs, then subclass
            // to override the still-light background fill.
            HWND hdr = reinterpret_cast<HWND>(
                ::SendMessageW(child, LVM_GETHEADER, 0, 0));
            if (hdr) {
                ::SetWindowTheme(hdr, L"DarkMode_ItemsView", nullptr);
                SubclassHeader(hdr);
            }
        } else if (lstrcmpiW(cls, L"SysHeader32") == 0) {
            // Bare headers (not nested inside a listview) — same fix.
            ::SetWindowTheme(child, L"DarkMode_ItemsView", nullptr);
            SubclassHeader(child);
        }
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
