#pragma once

#include <Windows.h>

namespace openzip::dark_theme {

bool IsDarkModeActive();   // reads HKCU\...\AppsUseLightTheme; cached
void Reset();              // re-reads cache (call on WM_SETTINGCHANGE)

void EnableForWindow(HWND);                         // titlebar (Win11) + recursive children
HBRUSH OnCtlColor(HWND ctl, HDC hdc, UINT nMsg);    // returns dark brush or NULL
LRESULT OnProgressCustomDraw(NMHDR* hdr);            // for CProgressCtrl NM_CUSTOMDRAW

// Subclass a SysHeader32 child so it renders via our owner-draw routine.
// The proc paints in BOTH light and dark modes (palette chosen at WM_PAINT
// time from IsDarkModeActive), so call this regardless of theme — it is
// what gives the light-mode header the same flat look that flat_button
// gives push buttons in light mode. Idempotent: re-subclassing is a no-op.
void SubclassHeader(HWND header);

}  // namespace openzip::dark_theme
