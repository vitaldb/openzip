#include "test_helpers.h"
#include <gtest/gtest.h>

#include "core/compressor.h"
#include "core/extractor.h"
#include "core/xz_archive.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

class NullCompressCb : public openzip::Compressor::ProgressCallback {
public:
    void OnEntryStart(const std::wstring&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override {}
    openzip::Extractor::ConflictAction OnOutputExists(const fs::path&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Compressor::Result) override {}
};

class NullExtractCb : public openzip::Extractor::ProgressCallback {
public:
    void OnEntryStart(const openzip::Extractor::Entry&, size_t, size_t) override {}
    void OnBytes(uint64_t, uint64_t) override {}
    std::wstring OnPasswordRequired(const std::wstring&, const std::wstring&, bool) override {
        return {};
    }
    openzip::Extractor::ConflictAction OnFileConflict(const std::wstring&) override {
        return openzip::Extractor::ConflictAction::Overwrite;
    }
    bool ShouldCancel() override { return false; }
    void OnComplete(openzip::Extractor::Result) override {}
};

}  // namespace

TEST(XzFormat, ExtensionDetection) {
    EXPECT_TRUE(openzip::xz::LooksLikeXz(L"foo.xz"));
    EXPECT_TRUE(openzip::xz::LooksLikeXz(L"FOO.XZ"));
    EXPECT_TRUE(openzip::xz::LooksLikeXz(L"foo.tar.xz"));
    EXPECT_TRUE(openzip::xz::LooksLikeXz(L"foo.txz"));
    EXPECT_FALSE(openzip::xz::LooksLikeXz(L"foo.gz"));
    EXPECT_FALSE(openzip::xz::LooksLikeXz(L"foo.zip"));
    EXPECT_FALSE(openzip::xz::LooksLikeXz(L"foo.txt"));
}

TEST(XzRoundtrip, SingleFileCompressAndExtract) {
    openzip::test::TempDir td;
    fs::path src = td / L"hello.txt";
    std::vector<uint8_t> payload;
    payload.reserve(8192);
    for (int i = 0; i < 8192; ++i) payload.push_back(static_cast<uint8_t>(i & 0xFF));
    openzip::test::WriteFileBytes(src, payload);

    fs::path xz = td / L"hello.txt.xz";
    NullCompressCb cb;
    auto cr = openzip::Compressor::Compress({src}, xz, cb);
    ASSERT_EQ(cr, openzip::Compressor::Result::Success);
    ASSERT_TRUE(fs::exists(xz));
    EXPECT_TRUE(openzip::xz::HasXzMagic(xz));

    auto entries = openzip::Extractor::ListEntries(xz);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, L"hello.txt");
    EXPECT_FALSE(entries[0].is_dir);
    // xz stores uncompressed size in the index; ListEntries should recover it.
    EXPECT_EQ(entries[0].uncompressed_size, payload.size());

    fs::path out_dir = td / L"out";
    NullExtractCb ecb;
    auto er = openzip::Extractor::Extract(xz, out_dir, ecb);
    ASSERT_EQ(er, openzip::Extractor::Result::Success);

    fs::path out_file = out_dir / L"hello.txt";
    ASSERT_TRUE(fs::exists(out_file));
    auto got = openzip::test::ReadFileBytes(out_file);
    ASSERT_EQ(got.size(), payload.size());
    EXPECT_EQ(got, payload);
}

TEST(XzCompress, FolderSourceFails) {
    openzip::test::TempDir td;
    fs::path src_dir = td / L"adir";
    fs::create_directories(src_dir);
    openzip::test::WriteFileBytes(src_dir / L"a.txt", {'a'});

    fs::path xz = td / L"out.xz";
    NullCompressCb cb;
    auto r = openzip::Compressor::Compress({src_dir}, xz, cb);
    EXPECT_EQ(r, openzip::Compressor::Result::IoError);
    EXPECT_FALSE(fs::exists(xz));
}

TEST(XzCompress, MultipleSourcesFails) {
    openzip::test::TempDir td;
    fs::path a = td / L"a.txt";
    fs::path b = td / L"b.txt";
    openzip::test::WriteFileBytes(a, {'a'});
    openzip::test::WriteFileBytes(b, {'b'});

    fs::path xz = td / L"out.xz";
    NullCompressCb cb;
    auto r = openzip::Compressor::Compress({a, b}, xz, cb);
    EXPECT_EQ(r, openzip::Compressor::Result::IoError);
    EXPECT_FALSE(fs::exists(xz));
}

TEST(XzExtract, EmptyPayloadRoundtrip) {
    openzip::test::TempDir td;
    fs::path src = td / L"empty.bin";
    openzip::test::WriteFileBytes(src, {});

    fs::path xz = td / L"empty.bin.xz";
    NullCompressCb ccb;
    ASSERT_EQ(openzip::Compressor::Compress({src}, xz, ccb),
              openzip::Compressor::Result::Success);

    fs::path out_dir = td / L"out";
    NullExtractCb ecb;
    ASSERT_EQ(openzip::Extractor::Extract(xz, out_dir, ecb),
              openzip::Extractor::Result::Success);
    ASSERT_TRUE(fs::exists(out_dir / L"empty.bin"));
    EXPECT_EQ(openzip::test::ReadFileBytes(out_dir / L"empty.bin").size(), 0u);
}

TEST(XzExtract, TxzStripsToTar) {
    openzip::test::TempDir td;
    fs::path src = td / L"payload.bin";
    openzip::test::WriteFileBytes(src, {'p','a','y','l','o','a','d'});

    fs::path txz = td / L"archive.txz";
    NullCompressCb ccb;
    ASSERT_EQ(openzip::Compressor::Compress({src}, txz, ccb),
              openzip::Compressor::Result::Success);

    auto entries = openzip::Extractor::ListEntries(txz);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, L"archive.tar");

    fs::path out_dir = td / L"out";
    NullExtractCb ecb;
    ASSERT_EQ(openzip::Extractor::Extract(txz, out_dir, ecb),
              openzip::Extractor::Result::Success);
    EXPECT_TRUE(fs::exists(out_dir / L"archive.tar"));
}
