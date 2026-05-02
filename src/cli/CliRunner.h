#pragma once

#include "CommandLine.h"

// Console-mode runner. Engaged by InitInstance when the process was launched
// from a parent that owns a console (cmd, PowerShell, Windows Terminal,
// Developer PowerShell etc.). The shell-extension and Explorer
// double-click paths bypass this entirely — they reach the GUI dispatch.
//
// CLI semantics:
//   openzip <zip>                       → list entries to stdout (unzip -l)
//   openzip --extract <zip> [--here|--folder|--target <dir>]
//                                       → extract to stdout-progress (no UI)
//   openzip --compress --output <zip> [...] --item <path> [...]
//                                       → compress with stdout-progress
//   openzip --help                      → help text (already handled in main)
//
// Returns process exit code: 0 on success, non-zero on failure.

namespace openzip::cli {

int Run(const CommandLine& cl);

}  // namespace openzip::cli
