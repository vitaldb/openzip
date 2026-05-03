#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "filename_decoder.h"

namespace openzip {

class Extractor {
public:
    enum class Result {
        Success,
        Cancelled,
        BadPassword,
        IoError,
        CorruptArchive,
        BombRefused,
        UnsafePath,
        ReservedName,
        UnsafeEntry,    // symlink, reparse point, or other dangerous metadata
    };

    enum class ConflictAction {
        Overwrite,
        Skip,
        Rename,  // extractor picks a non-conflicting name (foo.txt → foo (1).txt)
        Cancel,
    };

    struct Entry {
        std::wstring name;            // decoded filename
        DecodeSource decode_source;   // for diagnostics / UI tooltip
        uint64_t uncompressed_size = 0;
        uint64_t compressed_size = 0;
        // Last-modified / created time as Unix timestamps (seconds since
        // 1970-01-01 UTC). 0 if the entry has no recorded date or it's invalid.
        std::time_t modified_time = 0;
        std::time_t created_time  = 0;
        bool is_dir = false;
        bool is_symlink = false;      // Unix symlink (mode 0o120000) or Windows reparse point
        bool needs_password = false;
    };

    // Caller-supplied callback for progress, password prompts, conflicts, and cancellation.
    // All methods are invoked from the Extract() worker thread.
    class ProgressCallback {
    public:
        virtual ~ProgressCallback() = default;

        virtual void OnEntryStart(const Entry& entry, size_t index, size_t total_entries) = 0;
        virtual void OnBytes(uint64_t bytes_done, uint64_t total_bytes) = 0;

        // Return the password to retry with. Return empty string to cancel the operation.
        // was_wrong: true if a previous attempt with a non-empty password failed.
        virtual std::wstring OnPasswordRequired(const std::wstring& archive_name,
                                                const std::wstring& entry_name,
                                                bool was_wrong) = 0;

        virtual ConflictAction OnFileConflict(const std::wstring& dest_path) = 0;

        virtual bool ShouldCancel() = 0;

        virtual void OnComplete(Result result) = 0;
    };

    struct Options {
        // 0 = auto (max(1, hardware_concurrency)), 1 = legacy single-thread,
        // N>=2 = N worker threads. Internally clamped to [1, 16].
        // Encrypted archives always run single-threaded regardless (preserves
        // password-retry behavior).
        int concurrency = 0;

        // If non-empty, only entries whose decoded `name` is in this set are
        // extracted; all other entries are skipped. Empty (default) = extract
        // everything. Used by the archive browser dialog to extract a single
        // file or a selected subset.
        std::vector<std::wstring> include;
    };

    // List all entries without extracting. Returns an empty vector on open failure.
    static std::vector<Entry> ListEntries(const std::filesystem::path& zip_path);

    // Extract every entry from `zip_path` into `target_dir`. Creates target_dir if missing.
    static Result Extract(const std::filesystem::path& zip_path,
                          const std::filesystem::path& target_dir,
                          ProgressCallback& cb,
                          const Options& opts = {});

    // Decompression bomb thresholds (see plan §8). Static so tests can override if needed.
    //
    // The guard fires when the total compressed → uncompressed ratio is
    // pathological (>= kBombRatioLimit) AND any of the following holds:
    //   - total uncompressed size >= kBombSizeLimit
    //   - any single entry's uncompressed size >= kBombPerEntryLimit
    //
    // Numbers chosen so that legitimate archives (large media folders, code
    // repos, virtual disk images) pass while classic 42.zip-style bombs
    // (ratio ≫ 100) are caught even when their post-decompression size is
    // well under the 10 GiB total threshold.
    static constexpr uint64_t kBombRatioLimit     = 100;                            // 100×
    static constexpr uint64_t kBombSizeLimit      = 10ULL * 1024 * 1024 * 1024;     // 10 GiB total
    static constexpr uint64_t kBombPerEntryLimit  = 4ULL  * 1024 * 1024 * 1024;     // 4 GiB per entry
};

const char* ResultName(Extractor::Result r);

}  // namespace openzip
