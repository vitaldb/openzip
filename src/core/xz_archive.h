#pragma once

#include <filesystem>
#include <string>

#include "compressor.h"
#include "extractor.h"

namespace openzip {

// Single-stream xz support (XZ File Format 1.0.4 / liblzma).
//
// xz is a single compressed stream — no entry table, no per-file metadata.
// We expose it through the same Extractor/Compressor contract by
// synthesising a one-element entry whose name is derived from the archive
// filename (xz stores no FNAME-equivalent). `.tar.xz` / `.txz` decompress to
// a `.tar` payload; OpenZip writes that verbatim and stops there.
namespace xz {

// Recognised: `*.xz`, `*.txz`. Case-insensitive. Does not open the file.
bool LooksLikeXz(const std::filesystem::path& p);

// True iff the first six bytes of `p` match the xz magic FD 37 7A 58 5A 00.
bool HasXzMagic(const std::filesystem::path& p);

// Build a 1-element entry vector for an xz archive. Returns empty if the
// file cannot be opened.
std::vector<Extractor::Entry> ListEntries(const std::filesystem::path& xz_path);

// Decompress `xz_path` into `target_dir`. Honours the same callback contract
// as Extractor::Extract.
Extractor::Result Extract(const std::filesystem::path& xz_path,
                          const std::filesystem::path& target_dir,
                          Extractor::ProgressCallback& cb);

// Compress `source` (must be a single regular file) into `output_xz`.
Compressor::Result Compress(const std::filesystem::path& source,
                            const std::filesystem::path& output_xz,
                            Compressor::ProgressCallback& cb,
                            const Compressor::Options& opts);

}  // namespace xz
}  // namespace openzip
