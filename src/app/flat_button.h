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
