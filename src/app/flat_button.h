#pragma once

#include <Windows.h>

// Flat push-button paint via SetWindowSubclass. Renders a borderless
// filled rectangle with a single 1px outline, hover/pressed shading,
// and a dotted focus rect for keyboard navigation. Theme-aware: picks
// colors from openzip::dark_theme::IsDarkModeActive().
//
// Usage: from each dialog's OnInitDialog (after the existing dark-theme
// hook), call openzip::flat::ApplyToDialog(GetSafeHwnd()).

namespace openzip::flat {

void ApplyToDialog(HWND dlg);

}  // namespace openzip::flat

namespace openzip::icons {

// Set the dialog's title-bar (small) and taskbar (big) icons by loading
// the named resource at the exact pixel sizes the OS asks for. Avoids
// GDI scaling a 32x32 master frame down to 16x16 (which is blurry) by
// selecting the matching frame from a multi-res .ico.
void ApplyDialogIcon(HWND dlg, HINSTANCE module, int icon_id);

}  // namespace openzip::icons
