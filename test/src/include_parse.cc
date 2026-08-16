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
// IncludeParse suite
TEST(IncludeParse, ParseDirectiveReadsKeywordAndAfter) {
  auto d = parse_directive("#include <vector>");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->keyword, "include");
  EXPECT_EQ(d->after, "<vector>");

  auto blank = parse_directive("  #ifdef FOO");
  ASSERT_TRUE(blank.has_value());
  EXPECT_EQ(blank->keyword, "ifdef");
  EXPECT_EQ(blank->after, "FOO");

  EXPECT_FALSE(parse_directive("int x;").has_value());
  EXPECT_FALSE(parse_directive("").has_value());
}

TEST(IncludeParse, ParseDirectiveHashWsBehavior) {
  // skip_hash_ws=false does not consume a space between '#' and the keyword,
  // so `# include` yields an empty keyword (nullopt). A normal `#include`
  // still parses.
  EXPECT_FALSE(parse_directive("# include <x>", /*skip_hash_ws=*/false)
                   .has_value());
  auto no_ws = parse_directive("#include <x>", /*skip_hash_ws=*/false);
  ASSERT_TRUE(no_ws.has_value());
  EXPECT_EQ(no_ws->keyword, "include");
  EXPECT_EQ(no_ws->after, "<x>");
}

TEST(IncludeParse, ParseIncludeLineHandlesBothQuoteStyles) {
  auto q = parse_include_line("#include \"lib/foo.h\"");
  ASSERT_TRUE(q.has_value());
  EXPECT_EQ(q->path, "lib/foo.h");
  EXPECT_TRUE(q->is_quoted);

  auto s = parse_include_line("#include <vector>");
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->path, "vector");
  EXPECT_FALSE(s->is_quoted);

  EXPECT_FALSE(parse_include_line("#define X 1").has_value());
  EXPECT_FALSE(parse_include_line("#include").has_value());
  EXPECT_FALSE(parse_include_line("int x;").has_value());
}

TEST(IncludeParse, ParseIncludesCollectsAllIncludes) {
  auto incs = parse_includes(
      "#pragma once\n"
      "#include \"a.h\"\n"
      "#include <b.h>\n"
      "int x;\n"
      "#include \"c.h\"\n");
  ASSERT_EQ(incs.size(), 3u);
  EXPECT_EQ(incs[0].path, "a.h");
  EXPECT_TRUE(incs[0].is_quoted);
  EXPECT_EQ(incs[1].path, "b.h");
  EXPECT_FALSE(incs[1].is_quoted);
  EXPECT_EQ(incs[2].path, "c.h");
}

TEST(IncludeParse, IsXmacroIncludeDetectsIncAndDef) {
  EXPECT_TRUE(is_xmacro_include("details.inc"));
  EXPECT_TRUE(is_xmacro_include("tokens.def"));
  EXPECT_FALSE(is_xmacro_include("header.h"));
  EXPECT_FALSE(is_xmacro_include("source.cc"));
}

TEST(IncludeParse, ResolveIncludeFindsRelativeAndIncludeDirs) {
  // Relative to the including header's directory.
  auto rel = resolve_include("prod.h", data_path("adlreach_lib/use.h"), {});
  EXPECT_NE(rel, "");
  // Via a -I directory.
  auto via_i = resolve_include("adlreach_lib/prod.h", "unrelated.h",
                               {"-I", gDataDir});
  EXPECT_NE(via_i, "");
  EXPECT_NE(via_i.find("adlreach_lib/prod.h"), std::string::npos)
      << "the resolved path should point at the requested header";
  // Unknown include resolves to empty.
  EXPECT_EQ(resolve_include("does_not_exist.h", data_path("adlreach_lib/use.h"),
                            {"-I", gDataDir}),
            "");
}

TEST(IncludeParse, AnnotateGuardsCombinesElifBranchConditions) {
  // A branch past the first is reached only when every branch before it was
  // not, so its guard is its own condition plus their negations. A dispatch
  // header — one implementation per branch, all defining the same entities —
  // depends on that: without the negated priors the alternatives all look
  // unconditional and land in the same translation unit at once.
  std::string src =
      "#if defined(A)\n"
      "#include \"a.h\"\n"
      "#elif defined(B)\n"
      "#include \"b.h\"\n"
      "#else\n"
      "#include \"c.h\"\n"
      "#endif\n";
  auto incs = parse_includes(src);
  annotate_guards(src, incs);
  ASSERT_EQ(incs.size(), 3u);
  // a.h is under `#if defined(A)` — its guard_stack carries that guard.
  EXPECT_EQ(incs[0].guard_stack.size(), 1u);
  EXPECT_NE(incs[0].guard_stack[0].find("#if defined(A)"),
            std::string::npos);
  EXPECT_FALSE(incs[1].skip_gmf);
  ASSERT_EQ(incs[1].guard_stack.size(), 1u);
  EXPECT_EQ(incs[1].guard_stack[0], "#if !(defined(A)) && (defined(B))\n")
      << "an #elif include carries its own condition and the one it followed";
  EXPECT_FALSE(incs[2].skip_gmf);
  ASSERT_EQ(incs[2].guard_stack.size(), 1u);
  EXPECT_EQ(incs[2].guard_stack[0], "#if !(defined(A)) && !(defined(B))\n")
      << "an #else after an #elif negates every branch before it";
}

TEST(IncludeParse, AnnotateGuardsStripsFileIncludeGuard) {
  // The header's own `#ifndef X` / `#define X` include guard must not be
  // emitted as a GMF guard for an include inside it.
  std::string src =
      "#ifndef FOO_H_\n"
      "#define FOO_H_\n"
      "#include \"foo.h\"\n"
      "#endif  // FOO_H_\n";
  auto incs = parse_includes(src);
  annotate_guards(src, incs);
  ASSERT_EQ(incs.size(), 1u);
  EXPECT_TRUE(incs[0].guard_stack.empty())
      << "the file's own include guard must not wrap its includes";
}
