#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "extractor.h"  // for ConflictAction

namespace openzip {

class Compressor {
public:
    enum class Result {
        Success,
        Cancelled,
        IoError,
        SourceMissing,
        OutputExists,
        BadPassword,  // reserved
    };

    enum class Level { Store = 0, Fast = 1, Normal = 6, Max = 9 };
    enum class Encoding { Utf8, Cp949 };

    struct Options {
        Level level = Level::Normal;
        std::wstring password;                  // empty = no encryption
        Encoding filename_encoding = Encoding::Utf8;
        int concurrency = 0;                    // reserved for v1.x
    };

    class ProgressCallback {
    public:
        virtual ~ProgressCallback() = default;
        virtual void OnEntryStart(const std::wstring& src_relpath,
                                  size_t index, size_t total_entries) = 0;
        virtual void OnBytes(uint64_t done, uint64_t total) = 0;
        virtual Extractor::ConflictAction OnOutputExists(
            const std::filesystem::path& output_zip) = 0;
        virtual bool ShouldCancel() = 0;
        virtual void OnComplete(Result) = 0;
    };

    static Result Compress(const std::vector<std::filesystem::path>& sources,
                           const std::filesystem::path& output_zip,
                           ProgressCallback& cb,
                           const Options& opts = {});
};

const char* CompressResultName(Compressor::Result r);

}  // namespace openzip
