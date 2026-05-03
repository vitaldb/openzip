#include "stdafx.h"
#include "flat_button.h"
#include "dark_theme.h"

#include <CommCtrl.h>
#include <Uxtheme.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace openzip::flat {

namespace {

constexpr UINT_PTR kSubId = 0x0F1A7BUL;

struct State {
    bool hovered  = false;
    bool pressed  = false;
    bool tracking = false;
};

struct Palette {
    COLORREF bg, bg_hover, bg_pressed;
    COLORREF fg, fg_disabled;
    COLORREF border, border_default, border_focus;
};

Palette MakePalette() {
    Palette p{};
    if (openzip::dark_theme::IsDarkModeActive()) {
        p.bg              = RGB(38,  38,  38);
        p.bg_hover        = RGB(50,  50,  50);
        p.bg_pressed      = RGB(60,  60,  60);
        p.fg              = RGB(225, 225, 225);
        p.fg_disabled     = RGB(110, 110, 110);
        p.border          = RGB(72,  72,  72);
        p.border_default  = RGB(96, 156, 230);
        p.border_focus    = RGB(150, 150, 150);
    } else {
        p.bg              = RGB(252, 252, 252);
        p.bg_hover        = RGB(243, 243, 243);
        p.bg_pressed      = RGB(228, 228, 228);
        p.fg              = RGB(20,  20,  20);
        p.fg_disabled     = RGB(165, 165, 165);
        p.border          = RGB(195, 195, 195);
        p.border_default  = RGB(0,   120, 212);
        p.border_focus    = RGB(80,  80,  80);
    }
    return p;
}

void Paint(HWND btn, State* st, HDC override_dc = nullptr) {
    // Two callers: WM_PAINT uses BeginPaint/EndPaint; WM_PRINTCLIENT (and the
    // BeginBufferedAnimation snapshots that themed buttons use during click
    // animations) supply the target DC in wParam — painting to BeginPaint's
    // DC in that case would draw to the wrong surface and the animation
    // frames would fall back to the system look.
    PAINTSTRUCT ps{};
    HDC dc = override_dc;
    if (!dc) dc = ::BeginPaint(btn, &ps);
    if (!dc) return;

    RECT rc; ::GetClientRect(btn, &rc);
    Palette pal = MakePalette();
    LONG_PTR style = ::GetWindowLongPtr(btn, GWL_STYLE);
    bool enabled = ::IsWindowEnabled(btn) != FALSE;
    bool defbtn  = (style & BS_DEFPUSHBUTTON) != 0;
    bool focused = (::GetFocus() == btn);

    // Background
    COLORREF bg = pal.bg;
    if (enabled) {
        if (st->pressed)      bg = pal.bg_pressed;
        else if (st->hovered) bg = pal.bg_hover;
    }
    HBRUSH br = ::CreateSolidBrush(bg);
    ::FillRect(dc, &rc, br);
    ::DeleteObject(br);

    // 1px outline. Default button gets the accent-colored border;
    // focused-but-not-default gets a slightly stronger neutral border.
    COLORREF bord = pal.border;
    if (defbtn)        bord = pal.border_default;
    else if (focused)  bord = pal.border_focus;
    HPEN pen     = ::CreatePen(PS_SOLID, 1, bord);
    HPEN old_pen = static_cast<HPEN>(::SelectObject(dc, pen));
    HBRUSH old_br = static_cast<HBRUSH>(::SelectObject(dc, ::GetStockObject(NULL_BRUSH)));
    ::Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    ::SelectObject(dc, old_pen);
    ::SelectObject(dc, old_br);
    ::DeleteObject(pen);

    // Caption
    wchar_t text[256] = {};
    ::GetWindowTextW(btn, text, 256);
    ::SetTextColor(dc, enabled ? pal.fg : pal.fg_disabled);
    ::SetBkMode(dc, TRANSPARENT);
    HFONT font     = reinterpret_cast<HFONT>(::SendMessageW(btn, WM_GETFONT, 0, 0));
    HFONT old_font = font ? static_cast<HFONT>(::SelectObject(dc, font)) : nullptr;
    UINT  fmt      = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    ::DrawTextW(dc, text, -1, &rc, fmt);
    if (old_font) ::SelectObject(dc, old_font);

    // Focus rect (keyboard focus indicator). Inset 3px from the border.
    if (focused) {
        RECT fr = { rc.left + 3, rc.top + 3, rc.right - 3, rc.bottom - 3 };
        ::DrawFocusRect(dc, &fr);
    }

    if (!override_dc) ::EndPaint(btn, &ps);
}

LRESULT CALLBACK Proc(HWND btn, UINT msg, WPARAM wp, LPARAM lp,
                      UINT_PTR id, DWORD_PTR data) {
    auto* st = reinterpret_cast<State*>(data);

    switch (msg) {
        case WM_PAINT:
            Paint(btn, st);
            return 0;

        case WM_PRINTCLIENT:
            // UxTheme's BeginBufferedPaint / BeginBufferedAnimation composite
            // themed buttons via this message — wParam is the target DC of
            // the back buffer / animation snapshot, so paint to that DC
            // (not BeginPaint's). Without this, the click-press animation
            // falls back to the system themed look mid-frame.
            Paint(btn, st, reinterpret_cast<HDC>(wp));
            return 0;

        case WM_SETTEXT: {
            // Themed buttons can short-circuit a text change by drawing
            // directly to the window DC (bypassing WM_PAINT), reverting the
            // control to the system look. Let the BUTTON class store the
            // new caption, then force a full repaint through our subclass.
            LRESULT r = ::DefSubclassProc(btn, msg, wp, lp);
            ::InvalidateRect(btn, nullptr, TRUE);
            return r;
        }

        case WM_ENABLE:
            // Same risk on enable/disable transitions: themed BUTTON can
            // repaint without going through our WM_PAINT.
            ::InvalidateRect(btn, nullptr, TRUE);
            break;

        case WM_ERASEBKGND:
            // We fully repaint; suppress default erase to avoid flicker.
            return 1;

        case WM_MOUSEMOVE:
            if (!st->tracking) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, btn, 0 };
                ::TrackMouseEvent(&tme);
                st->tracking = true;
            }
            if (!st->hovered) {
                st->hovered = true;
                ::InvalidateRect(btn, nullptr, FALSE);
            }
            break;

        case WM_MOUSELEAVE:
            st->hovered  = false;
            st->pressed  = false;
            st->tracking = false;
            ::InvalidateRect(btn, nullptr, FALSE);
            break;

        case WM_LBUTTONDOWN:
            st->pressed = true;
            ::InvalidateRect(btn, nullptr, FALSE);
            break;

        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (st->pressed) {
                st->pressed = false;
                ::InvalidateRect(btn, nullptr, FALSE);
            }
            break;

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            ::InvalidateRect(btn, nullptr, FALSE);
            break;

        case WM_NCDESTROY:
            ::RemoveWindowSubclass(btn, Proc, id);
            delete st;
            break;
    }
    return ::DefSubclassProc(btn, msg, wp, lp);
}

}  // anonymous namespace

void ApplyToDialog(HWND dlg) {
    if (!dlg) return;
    ::EnumChildWindows(dlg, [](HWND child, LPARAM) -> BOOL {
        wchar_t cls[64] = {};
        ::GetClassNameW(child, cls, 64);
        if (lstrcmpiW(cls, L"Button") != 0) return TRUE;

        // Subclass push buttons only (BS_PUSHBUTTON / BS_DEFPUSHBUTTON).
        // Leave checkboxes, radios, group boxes alone — their themed look
        // is already flat enough, and owner-drawing them correctly across
        // light/dark + focus/hover states is significantly more code.
        DWORD type = static_cast<DWORD>(::GetWindowLongPtr(child, GWL_STYLE)) & BS_TYPEMASK;
        if (type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON) return TRUE;

        // Avoid double-subclassing if ApplyToDialog runs more than once.
        DWORD_PTR existing = 0;
        if (::GetWindowSubclass(child, Proc, kSubId, &existing) && existing) return TRUE;

        ::SetWindowSubclass(child, Proc, kSubId,
                            reinterpret_cast<DWORD_PTR>(new State()));
        ::InvalidateRect(child, nullptr, TRUE);
        return TRUE;
    }, 0);
}

}  // namespace openzip::flat

namespace openzip::icons {

void ApplyDialogIcon(HWND dlg, HINSTANCE module, int icon_id) {
    if (!dlg || !module) return;
    auto load = [&](int cx, int cy) -> HICON {
        return reinterpret_cast<HICON>(::LoadImageW(
            module, MAKEINTRESOURCEW(icon_id), IMAGE_ICON,
            cx, cy, LR_DEFAULTCOLOR));
    };
    if (HICON h = load(::GetSystemMetrics(SM_CXSMICON),
                       ::GetSystemMetrics(SM_CYSMICON))) {
        ::SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(h));
    }
    if (HICON h = load(::GetSystemMetrics(SM_CXICON),
                       ::GetSystemMetrics(SM_CYICON))) {
        ::SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(h));
    }
}

}  // namespace openzip::icons
