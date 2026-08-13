#include "gtest/gtest-macros.h"
import gtest;
import libtooling;
import modulizer;
import std;

namespace {

std::string gDataDir = TEST_DATA_DIR;

std::string data_path(llvm::StringRef fname) {
  return std::format("{}/{}", gDataDir, fname.str());
}

} // namespace
// Util suite
TEST(Util, FqnOfJoinsPathAndName) {
  EXPECT_EQ(fqn_of({"a", "b"}, "c"), "a::b::c");
  EXPECT_EQ(fqn_of({}, "c"), "c");
  EXPECT_EQ(fqn_of({"lib", "detail"}, "Foo"), "lib::detail::Foo");
}

TEST(Util, SplitOnDropsEmptySegments) {
  auto parts = split_on("a/b//c/", '/');
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], "a");
  EXPECT_EQ(parts[1], "b");
  EXPECT_EQ(parts[2], "c");
}

TEST(Util, SplitPathHandlesBothSeparators) {
  auto parts = split_path("a/b\\c");
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[2], "c");
}

TEST(Util, IsInternalSegmentMatchesMarkers) {
  // Exact-match semantics: a segment is internal only when it is a whole
  // `detail`/`internal`/`impl` name or starts with `_`. Substring matching
  // would misclassify e.g. `mylib_pred_impl.h` (public, merely contains
  // "impl") as internal.
  EXPECT_TRUE(is_internal_segment("internal"));
  EXPECT_TRUE(is_internal_segment("impl"));
  EXPECT_TRUE(is_internal_segment("detail"));
  EXPECT_TRUE(is_internal_segment("_helper"));
  EXPECT_FALSE(is_internal_segment("mylib-internal"));
  EXPECT_FALSE(is_internal_segment("api"));
}
