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
// Naming suite
TEST(Naming, MacroPrefixUpperSnakesDotsDashes) {
  EXPECT_EQ(macro_prefix("my_lib"), "MY_LIB");
  EXPECT_EQ(macro_prefix("my-lib"), "MY_LIB");
  EXPECT_EQ(macro_prefix("my.lib"), "MY_LIB");
  EXPECT_EQ(macro_prefix("a:b"), "A_B");
}

TEST(Naming, ParseInternalModeDefaultsToBoth) {
  EXPECT_EQ(parse_internal_mode("dir"), InternalMode::kDir);
  EXPECT_EQ(parse_internal_mode("stem"), InternalMode::kStem);
  EXPECT_EQ(parse_internal_mode("both"), InternalMode::kBoth);
  EXPECT_EQ(parse_internal_mode("bogus"), InternalMode::kBoth);
  EXPECT_EQ(parse_internal_mode(""), InternalMode::kBoth);
}

TEST(Naming, IsInternalModuleDirModeUsesDirectoriesOnly) {
  // dir mode: internal when a directory segment (not the stem) is a marker.
  EXPECT_TRUE(is_internal_module("lib/internal/api.h", "lib.internal.api",
                                 InternalMode::kDir));
  EXPECT_TRUE(is_internal_module("lib/impl/api.h", "lib.impl.api",
                                 InternalMode::kDir));
  // A public header whose STEM is internal does NOT count in dir mode.
  EXPECT_FALSE(is_internal_module("lib/a_internal.h", "lib.a_internal",
                                  InternalMode::kDir));
}

TEST(Naming, IsInternalModuleStemModeUsesStemOnly) {
  // stem mode: internal when the filename stem is a whole marker or `_`-prefixed.
  EXPECT_TRUE(is_internal_module("lib/_internal.h", "lib._internal",
                                 InternalMode::kStem));
  EXPECT_TRUE(is_internal_module("lib/_impl.h", "lib._impl",
                                 InternalMode::kStem));
  // An internal DIRECTORY does not count in stem mode.
  EXPECT_FALSE(is_internal_module("lib/internal/api.h", "lib.internal.api",
                                  InternalMode::kStem));
  // A public stem merely containing "internal" (e.g. mylib_pred_impl) is not
  // internal.
  EXPECT_FALSE(is_internal_module("lib/mylib_pred_impl.h", "lib.mylib_pred_impl",
                                  InternalMode::kStem));
}

TEST(Naming, IsInternalModuleBothModeAcceptsEither) {
  EXPECT_TRUE(is_internal_module("lib/internal/api.h", "lib.internal.api",
                                 InternalMode::kBoth));
  EXPECT_TRUE(is_internal_module("lib/_internal.h", "lib._internal",
                                 InternalMode::kBoth));
  EXPECT_FALSE(is_internal_module("lib/api.h", "lib.api",
                                  InternalMode::kBoth));
  // Default mode is kBoth.
  EXPECT_TRUE(is_internal_module("lib/internal/api.h", "lib.internal.api"));
}

TEST(Naming, DeriveModuleNameFromLibraryRoot) {
  // A header whose stem equals the library name is the top-level module.
  EXPECT_EQ(derive_module_name("mylib/mylib.h", "mylib"), "mylib");
  EXPECT_EQ(derive_module_name("mylib/api.h", "mylib"), "mylib.api");
  EXPECT_EQ(derive_module_name("mylib/core/api.h", "mylib"), "mylib.core.api");
  EXPECT_EQ(derive_module_name("mylib/internal/foo.h", "mylib"),
            "mylib.internal.foo");
  // The library root is found by the first path segment equal to the library
  // name, so an enclosing directory is ignored.
  EXPECT_EQ(derive_module_name("/src/mylib/api.h", "mylib"), "mylib.api");
}

TEST(Naming, DeriveModuleNameEscapesKeywordComponents) {
  // A module name is a list of identifiers, and `alignof` is not one. Left
  // alone, `export module mylib.alignof;` declares no module at all and the
  // build stops at "does not provide a module interface unit".
  EXPECT_EQ(derive_module_name("mylib/alignof.h", "mylib"), "mylib.alignof_");
  EXPECT_EQ(derive_module_name("mylib/detail/static_assert.h", "mylib"),
            "mylib.detail.static_assert_");
  EXPECT_EQ(derive_module_name("elsewhere/typename.h", "mylib"),
            "mylib.typename_");
  // Only exact collisions: a name that merely contains one is fine.
  EXPECT_EQ(derive_module_name("mylib/alignof_helper.h", "mylib"),
            "mylib.alignof_helper");
}

TEST(Naming, DeriveModuleNameFallsBackToStem) {
  // When no path segment matches the library name, fall back to stem naming
  // with dashes replaced by underscores.
  EXPECT_EQ(derive_module_name("other/foo-bar.h", "mylib"), "mylib.foo_bar");
  EXPECT_EQ(derive_module_name("other/api.h", "mylib"), "mylib.api");
  // A stem equal to the library name collapses to just the library name.
  EXPECT_EQ(derive_module_name("other/mylib.h", "mylib"), "mylib");
}
