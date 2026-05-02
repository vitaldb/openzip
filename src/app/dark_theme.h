#pragma once

#include <Windows.h>

namespace openzip::dark_theme {

bool IsDarkModeActive();   // reads HKCU\...\AppsUseLightTheme; cached
void Reset();              // re-reads cache (call on WM_SETTINGCHANGE)

void EnableForWindow(HWND);                         // titlebar (Win11) + recursive children
HBRUSH OnCtlColor(HWND ctl, HDC hdc, UINT nMsg);    // returns dark brush or NULL
LRESULT OnProgressCustomDraw(NMHDR* hdr);            // for CProgressCtrl NM_CUSTOMDRAW

}  // namespace openzip::dark_theme
