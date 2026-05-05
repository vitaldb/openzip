#pragma once

// Single source of truth for the OpenZip version string.
// Read by msix/build_msix.py to stamp the MSIX <Identity Version="…">,
// and (if/when wired) by the C++ build to populate the .rc VERSIONINFO.
#define OPENZIP_VERSION "0.5.0"
