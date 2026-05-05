#pragma once

#include <filesystem>
#include <string>

#include "compressor.h"
#include "extractor.h"

namespace openzip {

// Single-stream gzip support (RFC 1952). A `.gz` file is one compressed
// payload, not a multi-entry archive — so the API mimics ZIP semantics with
// exactly one synthetic entry whose name is recovered from the gzip FNAME
// header (or, failing that, derived by stripping the `.gz` suffix).
//
// `.tar.gz` / `.tgz` are not unpacked here; they decompress to a `.tar`
// payload, which OpenZip then writes verbatim. Users who want individual
// files out of a tarball still get a usable intermediate.
namespace gzip {

// True if `p` looks like a gzip stream by extension. Case-insensitive.
// Recognised: `*.gz`, `*.tgz`, `*.taz`. Does not open the file.
bool LooksLikeGz(const std::filesystem::path& p);

// True iff the first two bytes of `p` are the gzip magic (0x1f 0x8b).
bool HasGzMagic(const std::filesystem::path& p);

// Build a 1-element entry vector for a `.gz` archive. The entry's name is
// taken from the gzip FNAME field if present, else the file's stem. Returns
// an empty vector if the file cannot be opened or is not a gzip stream.
std::vector<Extractor::Entry> ListEntries(const std::filesystem::path& gz_path);

// Decompress `gz_path` into `target_dir`. Honours the same callback contract
// as Extractor::Extract (progress, conflict, cancel). Always sequential —
// single-stream input gives no parallelism opportunity.
Extractor::Result Extract(const std::filesystem::path& gz_path,
                          const std::filesystem::path& target_dir,
                          Extractor::ProgressCallback& cb);

// Compress `source` (must be a single regular file) into `output_gz`.
// Folders are not supported here — Compressor::Compress catches that case
// and reports IoError before reaching this path.
Compressor::Result Compress(const std::filesystem::path& source,
                            const std::filesystem::path& output_gz,
                            Compressor::ProgressCallback& cb,
                            const Compressor::Options& opts);

}  // namespace gzip
}  // namespace openzip
