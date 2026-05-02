#pragma once

// Win11 22000+ per plan §2.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10/11
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10_CO  // Windows 11 22H2 features
#endif

#include <SDKDDKVer.h>
