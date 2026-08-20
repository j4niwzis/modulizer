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

TEST(Util, BaseCompileFlagsCarryClangsResourceDir) {
  // A ClangTool derives clang's builtin-header directory from the running
  // executable's path, so every parse fails with `'stddef.h' file not found`
  // unless the binary happens to sit beside a clang installation. That failure
  // is silent — no AST means nothing is found to be used, and every
  // transitively used header is then dropped from the generated output — so
  // the flags name the directory explicitly instead.
  auto &rd = clang_resource_dir();
  if (rd.empty()) GTEST_SKIP() << "no clang++ on PATH to ask for one";
  EXPECT_TRUE(std::filesystem::exists(std::format("{}/include/stddef.h", rd)))
      << "the resource directory must be the one holding clang's builtins";
  auto flags = base_compile_flags();
  EXPECT_TRUE(std::ranges::contains(flags, std::format("-resource-dir={}", rd)))
      << "every parse must be told where clang's builtin headers are";
}

// --module-replaces spells out a module that provides headers. One flag covers
// both shapes: a named module standing for a list of headers, and a whole
// converted library standing for everything under a path.

TEST(Util, ParsesAModuleStandingForNamedHeaders) {
  std::string err;
  auto r = parse_module_replacement("std=vector,string", err);
  ASSERT_TRUE(r.has_value()) << err;
  EXPECT_EQ(r->module, "std");
  EXPECT_EQ(r->headers, (std::set<std::string>{"vector", "string"}));
  EXPECT_TRUE(r->guard.empty()) << "nothing to switch off";
  EXPECT_FALSE(r->carries_macros_file);
}

TEST(Util, ParsesOneConvertedModuleAndTheHeaderItProvides) {
  // A converted library contributes one of these per module it generated, so
  // that the narrowest module providing a header is the one chosen.
  std::string err;
  auto r = parse_module_replacement("boost.mp11.list=boost/mp11/list.hpp", err);
  ASSERT_TRUE(r.has_value()) << err;
  EXPECT_EQ(r->module, "boost.mp11.list");
  EXPECT_EQ(r->headers, (std::set<std::string>{"boost/mp11/list.hpp"}));
}

TEST(Util, ParsesTheGuardAndMacrosOptions) {
  std::string err;
  auto r = parse_module_replacement(
      "boost.mp11.list=boost/mp11/list.hpp:guard=BOOST_MP11X_IMPORT_MODULES:macros",
      err);
  ASSERT_TRUE(r.has_value()) << err;
  EXPECT_EQ(r->guard, "BOOST_MP11X_IMPORT_MODULES");
  EXPECT_TRUE(r->carries_macros_file)
      << "a module carries no macros, so the provider's macros file travels "
         "with the import";
}

TEST(Util, ParsesAGuardOnANamedModuleToo) {
  // The options are not tied to the prefix form: a named module can be
  // switched off as well.
  std::string err;
  auto r = parse_module_replacement("mylib.thing=mylib/thing.hpp:guard=USE_IT",
                                    err);
  ASSERT_TRUE(r.has_value()) << err;
  EXPECT_EQ(r->module, "mylib.thing");
  EXPECT_EQ(r->headers, (std::set<std::string>{"mylib/thing.hpp"}));
  EXPECT_EQ(r->guard, "USE_IT");
}

TEST(Util, PrefersTheModuleStandingForTheFewestHeaders) {
  // `std` and `std.variant` both provide <variant>; the answer wanted is the
  // narrower one.
  ModuleReplacements reps = {
      ModuleReplacement{.module = "std", .headers = {"variant", "vector"}},
      ModuleReplacement{.module = "std.variant", .headers = {"variant"}},
  };
  auto *r = find_replacement("variant", reps);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->module, "std.variant");
  EXPECT_EQ(find_replacement("vector", reps)->module, "std");
  EXPECT_EQ(find_replacement("string", reps), nullptr);
}

TEST(Util, RejectsASpecWithNothingToReplace) {
  std::string err;
  EXPECT_FALSE(parse_module_replacement("std", err).has_value())
      << "no '=' at all";
  EXPECT_FALSE(err.empty()) << "and it must say why";
  err.clear();
  EXPECT_FALSE(parse_module_replacement("std=", err).has_value())
      << "a module replacing no headers replaces nothing";
  EXPECT_FALSE(err.empty());
  err.clear();
  EXPECT_FALSE(parse_module_replacement("m=a.hpp:guard=", err).has_value())
      << "an empty guard would read as unconditional, which is the opposite "
         "of what was asked for";
  EXPECT_FALSE(err.empty());
  err.clear();
  EXPECT_FALSE(parse_module_replacement("m=a.hpp:nonsense", err).has_value())
      << "an unknown option is a typo, not something to ignore";
  EXPECT_FALSE(err.empty());
  err.clear();
  EXPECT_FALSE(parse_module_replacement("=a.hpp", err).has_value())
      << "a replacement with no module names nothing to import";
  EXPECT_FALSE(err.empty());
}
