#include "test_helpers.h"
#include <gtest/gtest.h>

TEST(SmokeTest, TempDirRoundtrip) {
    openzip::test::TempDir td;
    auto p = td.path() / L"hello.txt";
    openzip::test::WriteFileBytes(p, {'h', 'i'});
    auto got = openzip::test::ReadFileBytes(p);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], 'h');
    EXPECT_EQ(got[1], 'i');
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
