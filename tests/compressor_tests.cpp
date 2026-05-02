#include "test_helpers.h"
#include <gtest/gtest.h>

#include "core/compressor.h"
#include "core/extractor.h"

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

// ---------------------------------------------------------------------------
// Shared null callback
// ---------------------------------------------------------------------------
namespace {
class NullCallback : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override {}
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result) override {}
};
}  // namespace

// ---------------------------------------------------------------------------
// Phase 0 smoke test (kept)
// ---------------------------------------------------------------------------
TEST(SmokeTest, TempDirRoundtrip) {
    openzip::test::TempDir td;
    auto p = td.path() / L"hello.txt";
    openzip::test::WriteFileBytes(p, {'h', 'i'});
    auto got = openzip::test::ReadFileBytes(p);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], 'h');
    EXPECT_EQ(got[1], 'i');
}

// ---------------------------------------------------------------------------
// Task 1.1
// ---------------------------------------------------------------------------
TEST(Compressor, EmptySourcesProducesEmptyValidZip) {
    openzip::test::TempDir td;
    fs::path zip = td / L"empty.zip";
    NullCallback cb;
    auto r = openzip::Compressor::Compress({}, zip, cb);
    ASSERT_EQ(r, openzip::Compressor::Result::Success);
    ASSERT_TRUE(fs::exists(zip));
    auto entries = openzip::Extractor::ListEntries(zip);
    EXPECT_EQ(entries.size(), 0u);
}

// ---------------------------------------------------------------------------
// Task 1.2
// ---------------------------------------------------------------------------
TEST(Compressor, SingleFileRoundtrip) {
    openzip::test::TempDir td;
    fs::path src_dir = td / L"src";
    fs::create_directories(src_dir);
    fs::path src_file = src_dir / L"hello.txt";
    openzip::test::WriteFileBytes(src_file, {'h','e','l','l','o'});

    fs::path zip = td / L"out.zip";
    NullCallback cb;
    auto r = openzip::Compressor::Compress({src_file}, zip, cb);
    ASSERT_EQ(r, openzip::Compressor::Result::Success);

    auto entries = openzip::Extractor::ListEntries(zip);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, L"hello.txt");
    EXPECT_EQ(entries[0].uncompressed_size, 5u);
}

// ---------------------------------------------------------------------------
// Task 1.3
// ---------------------------------------------------------------------------
TEST(Compressor, FolderRecursive) {
    openzip::test::TempDir td;
    fs::path root = td / L"MyDocs";
    openzip::test::WriteFileBytes(root / L"a.txt", {'a'});
    openzip::test::WriteFileBytes(root / L"sub" / L"b.txt", {'b'});

    fs::path zip = td / L"out.zip";
    NullCallback cb;
    auto r = openzip::Compressor::Compress({root}, zip, cb);
    ASSERT_EQ(r, openzip::Compressor::Result::Success);

    auto entries = openzip::Extractor::ListEntries(zip);
    std::vector<std::wstring> names;
    for (auto& e : entries) names.push_back(e.name);
    std::sort(names.begin(), names.end());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], L"MyDocs/a.txt");
    EXPECT_EQ(names[1], L"MyDocs/sub/b.txt");
}

// ---------------------------------------------------------------------------
// Task 1.4
// ---------------------------------------------------------------------------
TEST(Compressor, Cp949EncodingSetsRawBytesNoUtf8Flag) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"테스트.txt";  // 테스트.txt
    openzip::test::WriteFileBytes(src, {'x'});

    fs::path zip = td / L"out.zip";
    NullCallback cb;
    openzip::Compressor::Options opts;
    opts.filename_encoding = openzip::Compressor::Encoding::Cp949;
    auto r = openzip::Compressor::Compress({src}, zip, cb, opts);
    ASSERT_EQ(r, openzip::Compressor::Result::Success);

    // Open with raw minizip and inspect the entry's flag byte directly.
    void* reader = mz_zip_reader_create();
    ASSERT_EQ(mz_zip_reader_open_file(reader,
        openzip::test::WideToUtf8(zip.wstring()).c_str()), MZ_OK);
    ASSERT_EQ(mz_zip_reader_goto_first_entry(reader), MZ_OK);
    mz_zip_file* info = nullptr;
    ASSERT_EQ(mz_zip_reader_entry_get_info(reader, &info), MZ_OK);
    EXPECT_EQ(info->flag & MZ_ZIP_FLAG_UTF8, 0);
    // EUC-KR bytes for "테스트.txt" start with 0xC5 0xD7
    ASSERT_NE(info->filename, nullptr);
    EXPECT_EQ(static_cast<unsigned char>(info->filename[0]), 0xC5);
    EXPECT_EQ(static_cast<unsigned char>(info->filename[1]), 0xD7);
    mz_zip_reader_delete(&reader);
}

TEST(Compressor, Utf8EncodingDefaultSetsFlag) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"테스트.txt";  // 테스트.txt
    openzip::test::WriteFileBytes(src, {'x'});

    fs::path zip = td / L"out.zip";
    NullCallback cb;
    auto r = openzip::Compressor::Compress({src}, zip, cb);
    ASSERT_EQ(r, openzip::Compressor::Result::Success);

    void* reader = mz_zip_reader_create();
    ASSERT_EQ(mz_zip_reader_open_file(reader,
        openzip::test::WideToUtf8(zip.wstring()).c_str()), MZ_OK);
    ASSERT_EQ(mz_zip_reader_goto_first_entry(reader), MZ_OK);
    mz_zip_file* info = nullptr;
    ASSERT_EQ(mz_zip_reader_entry_get_info(reader, &info), MZ_OK);
    EXPECT_EQ(info->flag & MZ_ZIP_FLAG_UTF8, MZ_ZIP_FLAG_UTF8);
    mz_zip_reader_delete(&reader);
}

// ---------------------------------------------------------------------------
// Task 1.5
// ---------------------------------------------------------------------------
TEST(Compressor, StoreLevelLeavesEntryUncompressed) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"a.bin";
    std::vector<uint8_t> bytes(8192, 0xAB);  // highly compressible
    openzip::test::WriteFileBytes(src, bytes);

    fs::path zip = td / L"out.zip";
    NullCallback cb;
    openzip::Compressor::Options opts;
    opts.level = openzip::Compressor::Level::Store;
    ASSERT_EQ(openzip::Compressor::Compress({src}, zip, cb, opts),
              openzip::Compressor::Result::Success);

    auto entries = openzip::Extractor::ListEntries(zip);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].uncompressed_size, 8192u);
    EXPECT_EQ(entries[0].compressed_size, 8192u);
}

// ---------------------------------------------------------------------------
// Task 1.6
// ---------------------------------------------------------------------------
namespace {
class FixedPasswordCallback : public openzip::Extractor::ProgressCallback {
public:
    explicit FixedPasswordCallback(std::wstring pw) : pw_(std::move(pw)) {}
    void OnEntryStart(const openzip::Extractor::Entry&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override {}
    std::wstring OnPasswordRequired(const std::wstring&, const std::wstring&, bool) override {
        return pw_;
    }
    openzip::Extractor::ConflictAction OnFileConflict(const std::wstring&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Extractor::Result) override {}
private:
    std::wstring pw_;
};
}  // namespace

TEST(Compressor, AesPasswordRoundtrip) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"secret.txt";
    openzip::test::WriteFileBytes(src, {'t','o','p','s','e','c'});

    fs::path zip = td / L"out.zip";
    NullCallback cb_w;
    openzip::Compressor::Options opts;
    opts.password = L"P@ssw0rd!";
    ASSERT_EQ(openzip::Compressor::Compress({src}, zip, cb_w, opts),
              openzip::Compressor::Result::Success);

    auto entries = openzip::Extractor::ListEntries(zip);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_TRUE(entries[0].needs_password);

    // Fix B: verify AES-256 (not ZipCrypto) is used in the stored entry.
    // Note: minizip-ng's mz_zip_reader_entry_get_info returns the actual
    // compression method from the AES extra field (e.g. DEFLATE=8), NOT 99.
    // The reliable AES indicator is aes_version != 0.
    {
        void* reader = mz_zip_reader_create();
        ASSERT_EQ(mz_zip_reader_open_file(reader,
            openzip::test::WideToUtf8(zip.wstring()).c_str()), MZ_OK);
        ASSERT_EQ(mz_zip_reader_goto_first_entry(reader), MZ_OK);
        mz_zip_file* info = nullptr;
        ASSERT_EQ(mz_zip_reader_entry_get_info(reader, &info), MZ_OK);
        // aes_version != 0 proves AES (not ZipCrypto) was used
        EXPECT_EQ(info->aes_version, MZ_AES_VERSION);
        // compression_method here is the actual inner method (DEFLATE=8), not 99
        EXPECT_EQ(info->compression_method, MZ_COMPRESS_METHOD_DEFLATE);
        mz_zip_reader_delete(&reader);
    }

    fs::path out_dir = td / L"out";
    FixedPasswordCallback cb_r(L"P@ssw0rd!");
    auto er = openzip::Extractor::Extract(zip, out_dir, cb_r);
    ASSERT_EQ(er, openzip::Extractor::Result::Success);
    auto got = openzip::test::ReadFileBytes(out_dir / L"secret.txt");
    EXPECT_EQ(got.size(), 6u);
    EXPECT_EQ(got[0], 't');
}

// ---------------------------------------------------------------------------
// Task 1.7
// ---------------------------------------------------------------------------
namespace {
class CancelAfterFirstCallback : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t i, size_t) override {
        if (i >= 1) cancel_ = true;
    }
    void OnBytes(uint64_t, uint64_t) override {}
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return cancel_; }
    void OnComplete(openzip::Compressor::Result r) override { result_ = r; }
    openzip::Compressor::Result result_ = openzip::Compressor::Result::Success;
    bool cancel_ = false;
};
}  // namespace

TEST(Compressor, CancelDeletesPartialNoFinalZip) {
    openzip::test::TempDir td;
    openzip::test::WriteFileBytes(td / L"src" / L"a.txt", {'a'});
    openzip::test::WriteFileBytes(td / L"src" / L"b.txt", {'b'});

    fs::path zip = td / L"out.zip";
    CancelAfterFirstCallback cb;
    auto r = openzip::Compressor::Compress(
        {td / L"src" / L"a.txt", td / L"src" / L"b.txt"}, zip, cb);
    EXPECT_EQ(r, openzip::Compressor::Result::Cancelled);
    EXPECT_FALSE(fs::exists(zip));
    fs::path partial = zip; partial += L".partial";
    EXPECT_FALSE(fs::exists(partial));
}

// ---------------------------------------------------------------------------
// Task 1.8
// ---------------------------------------------------------------------------
namespace {
class SkipOnConflict : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override {}
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Skip;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result r) override { last_ = r; }
    openzip::Compressor::Result last_ = openzip::Compressor::Result::Success;
};

class RenameOnConflict : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override {}
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Rename;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result) override {}
};
}  // namespace

TEST(Compressor, OutputExistsSkipPreservesOriginal) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"a.txt";
    openzip::test::WriteFileBytes(src, {'a'});
    fs::path zip = td / L"out.zip";
    openzip::test::WriteFileBytes(zip, {'O','L','D'});  // pre-existing file

    SkipOnConflict cb;
    auto r = openzip::Compressor::Compress({src}, zip, cb);
    EXPECT_EQ(r, openzip::Compressor::Result::OutputExists);
    auto bytes = openzip::test::ReadFileBytes(zip);
    EXPECT_EQ(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 'O');
}

TEST(Compressor, OutputExistsRenameWritesSuffixedFile) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"a.txt";
    openzip::test::WriteFileBytes(src, {'a'});
    fs::path zip = td / L"out.zip";
    openzip::test::WriteFileBytes(zip, {'O','L','D'});

    RenameOnConflict cb;
    auto r = openzip::Compressor::Compress({src}, zip, cb);
    EXPECT_EQ(r, openzip::Compressor::Result::Success);
    EXPECT_TRUE(fs::exists(td / L"out (1).zip"));
    auto orig = openzip::test::ReadFileBytes(zip);
    EXPECT_EQ(orig[0], 'O');
}

// ---------------------------------------------------------------------------
// Task 1.9
// ---------------------------------------------------------------------------
namespace {
class RecordingCallback : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t total) override {
        total_entries_ = total;
    }
    void OnBytes(uint64_t done, uint64_t total) override {
        last_done_ = done; last_total_ = total;
    }
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result) override {}
    size_t total_entries_ = 0;
    uint64_t last_done_ = 0, last_total_ = 0;
};
}  // namespace

TEST(Compressor, ProgressBytesReachTotalAtEnd) {
    openzip::test::TempDir td;
    std::vector<uint8_t> blob(50000, 0x42);
    openzip::test::WriteFileBytes(td / L"src" / L"a.bin", blob);
    openzip::test::WriteFileBytes(td / L"src" / L"b.bin", blob);

    fs::path zip = td / L"out.zip";
    RecordingCallback cb;
    ASSERT_EQ(openzip::Compressor::Compress(
        {td / L"src" / L"a.bin", td / L"src" / L"b.bin"}, zip, cb),
        openzip::Compressor::Result::Success);

    EXPECT_EQ(cb.total_entries_, 2u);
    EXPECT_EQ(cb.last_total_, 100000u);
    EXPECT_EQ(cb.last_done_, 100000u);
}

// ---------------------------------------------------------------------------
// Fix A: callback throws — writer and partial file must be cleaned up
// ---------------------------------------------------------------------------
namespace {
class ThrowOnEntryCallback : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t) override {
        throw std::runtime_error("boom");
    }
    void OnBytes(uint64_t, uint64_t) override {}
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result) override {}
};
}  // namespace

TEST(Compressor, ThrowingCallbackCleansUpPartial) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"a.txt";
    openzip::test::WriteFileBytes(src, {'a'});

    fs::path zip = td / L"out.zip";
    fs::path partial = zip;
    partial += L".partial";

    ThrowOnEntryCallback cb;
    try {
        openzip::Compressor::Compress({src}, zip, cb);
    } catch (const std::runtime_error&) {
        // expected — RAII guards must still have cleaned up
    }
    EXPECT_FALSE(fs::exists(zip));
    EXPECT_FALSE(fs::exists(partial));
}

// ---------------------------------------------------------------------------
// Fix D: cancel mid-write — Result::Cancelled, partial absent
// ---------------------------------------------------------------------------
namespace {
class CancelAfterFirstBytesCallback : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override { cancel_ = true; }
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return cancel_; }
    void OnComplete(openzip::Compressor::Result r) override { result_ = r; }
    openzip::Compressor::Result result_ = openzip::Compressor::Result::Success;
    bool cancel_ = false;
};
}  // namespace

TEST(Compressor, CancelMidWriteDeletesPartial) {
    openzip::test::TempDir td;
    // 5 MB source — large enough to need multiple 64 KB reads via manual path
    std::vector<uint8_t> big(5 * 1024 * 1024, 0xCC);
    fs::path src = td / L"src" / L"big.bin";
    openzip::test::WriteFileBytes(src, big);

    fs::path zip = td / L"out.zip";
    fs::path partial = zip;
    partial += L".partial";

    // Use CP949 path so WriteEntryManual's cancel check fires
    openzip::Compressor::Options opts;
    opts.filename_encoding = openzip::Compressor::Encoding::Cp949;

    CancelAfterFirstBytesCallback cb;
    auto r = openzip::Compressor::Compress({src}, zip, cb, opts);
    EXPECT_EQ(r, openzip::Compressor::Result::Cancelled);
    EXPECT_FALSE(fs::exists(zip));
    EXPECT_FALSE(fs::exists(partial));
}

// ---------------------------------------------------------------------------
// Fix F: SourceMissing test
// ---------------------------------------------------------------------------
TEST(Compressor, SourceMissingReturnsSourceMissing) {
    openzip::test::TempDir td;
    fs::path zip = td / L"out.zip";
    NullCallback cb;
    auto r = openzip::Compressor::Compress(
        {td / L"does-not-exist.txt"}, zip, cb);
    EXPECT_EQ(r, openzip::Compressor::Result::SourceMissing);
    EXPECT_FALSE(fs::exists(zip));
}

// ---------------------------------------------------------------------------
// Fix G: OutputExists + Cancel action
// ---------------------------------------------------------------------------
namespace {
class CancelOnConflict : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override {}
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Cancel;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result) override {}
};
}  // namespace

TEST(Compressor, OutputExistsCancelReturnsOutputExists) {
    openzip::test::TempDir td;
    fs::path src = td / L"src" / L"a.txt";
    openzip::test::WriteFileBytes(src, {'a'});
    fs::path zip = td / L"out.zip";
    openzip::test::WriteFileBytes(zip, {'O','L','D'});

    CancelOnConflict cb;
    auto r = openzip::Compressor::Compress({src}, zip, cb);
    EXPECT_EQ(r, openzip::Compressor::Result::OutputExists);
    auto bytes = openzip::test::ReadFileBytes(zip);
    EXPECT_EQ(bytes.size(), 3u);
}

// CoreTests links gtest.lib (not gtest_main.lib), so this main is required.
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
