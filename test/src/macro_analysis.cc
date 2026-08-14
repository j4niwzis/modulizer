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
// MacroAnalysis suite
TEST(MacroAnalysis, FindsQualifiedInternalName) {
  auto model = analyze_file(data_path("macro_test.h"));
  if (model.macros.empty()) { GTEST_SKIP(); return; }
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  bool has_detail = false;
  for (auto &f : r.reachable_fqns)
    if (f.find("detail::") != std::string::npos) has_detail = true;
  EXPECT_TRUE(has_detail);
}

TEST(MacroAnalysis, TransitiveReachability) {
  auto model = analyze_file(data_path("macro_test.h"));
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  bool found = false;
  for (auto &f : r.reachable_fqns)
    if (f.find("intermediate") != std::string::npos) found = true;
  EXPECT_TRUE(found);
}

TEST(MacroAnalysis, TokenPasteWildcard) {
  auto model = analyze_file(data_path("macro_test.h"));
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  EXPECT_GT(r.wildcards.size(), 0u);
}

TEST(MacroAnalysis, PublicMacroReferencesInternalClass) {
  auto model = analyze_file(data_path("macro_public_internal.h"));
  if (model.macros.empty()) { GTEST_SKIP(); return; }
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  bool has_foo = false;
  for (auto &f : r.reachable_fqns)
    if (f == "test_lib::detail::Foo") has_foo = true;
  EXPECT_TRUE(has_foo)
      << "internal class referenced by public macro must be reachable";
}

TEST(MacroAnalysis, KeywordBeforeQualifiedName) {
  auto model = analyze_file(data_path("macro_new_keyword.h"));
  if (model.macros.empty()) { GTEST_SKIP(); return; }
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  bool has_foo = false;
  for (auto &f : r.reachable_fqns)
    if (f == "test_lib::detail::Foo") has_foo = true;
  EXPECT_TRUE(has_foo)
      << "internal class after `new` keyword in macro must be reachable";
}

TEST(MacroAnalysis, PublicMacroReachesInternalEntityViaCallGraph) {
  // A public macro A calls private macro B, whose body names an internal
  // entity. The BFS from public macros must reach B and surface the FQN.
  EntityModel model;
  model.macros.push_back({"PUBLIC_A", "PUBLIC_B()", true, {"x"},
                          {"PUBLIC_B", "(", ")", }});
  model.macros.push_back({"PUBLIC_B", "detail::helper()", false, {},
                          {"detail", "::", "helper", "(", ")"}});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  bool found = false;
  for (auto &f : r.reachable_fqns)
    if (f == "detail::helper") found = true;
  EXPECT_TRUE(found)
      << "a private macro reached from a public macro must contribute its "
         "internal entities";
}

TEST(MacroAnalysis, PrivateUnderscoreMacroIsNotPublic) {
  // A macro whose name starts with `_` is not public: it is only analyzed when
  // a public macro reaches it, never as a seed.
  EntityModel model;
  model.macros.push_back({"_private_only", "detail::helper()", false, {},
                          {"detail", "::", "helper", "(", ")"}});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  bool found = false;
  for (auto &f : r.reachable_fqns)
    if (f == "detail::helper") found = true;
  EXPECT_FALSE(found)
      << "an underscore-prefixed macro is not a public seed";
}

TEST(MacroAnalysis, IncludeGuardMacroIsNotPublic) {
  // An ALL_CAPS macro ending in `_H_` / `_H` (an include guard) is not public
  // and must not seed reachability.
  EntityModel model;
  model.macros.push_back({"FOO_H_", "detail::helper()", false, {},
                          {"detail", "::", "helper", "(", ")"}});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  bool found = false;
  for (auto &f : r.reachable_fqns)
    if (f == "detail::helper") found = true;
  EXPECT_FALSE(found)
      << "an include-guard macro must not be a public seed";
}

TEST(MacroAnalysis, WildcardTokenPasteProducesPrefix) {
  // `detail::foo_ ## param` yields a wildcard `foo_` scoped to `detail`.
  EntityModel model;
  model.macros.push_back({"WILDCARD", "detail::foo_ ## name", true, {"name"},
                          {"detail", "::", "foo_", "##", "name"}});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  bool found = false;
  for (auto &wc : r.wildcards) {
    if (wc.first == "foo_" && wc.second.size() == 1 &&
        wc.second[0] == "detail")
      found = true;
  }
  EXPECT_TRUE(found)
      << "token-paste wildcard must be extracted with its namespace scope";
}

TEST(MacroAnalysis, ShallowestInternalPrefixIsReported) {
  // `lib::detail::sub::Foo` — the first entity whose namespace contains the
  // `detail` internal segment is `lib::detail::sub`, so reachable_fqns must
  // report that (not the deeper `Foo` or the bare `lib::detail`).
  EntityModel model;
  model.macros.push_back({"PUBLIC", "lib::detail::sub::Foo", false, {},
                          {"lib", "::", "detail", "::", "sub", "::", "Foo"}});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  bool found_prefix = false, found_full = false;
  for (auto &f : r.reachable_fqns) {
    if (f == "lib::detail::sub") found_prefix = true;
    if (f == "lib::detail::sub::Foo") found_full = true;
  }
  EXPECT_TRUE(found_prefix)
      << "the shallowest internal entity prefix must be reported";
  EXPECT_FALSE(found_full)
      << "only the shallowest internal prefix should be reported, not the "
         "deeper name";
}

TEST(MacroAnalysis, ExpandOverloadsAddsSiblingFunctions) {
  // `detail::foo` is reachable via a public macro. Overloads `foo(int)` and
  // `foo(double)` share the same namespace + name, so expand_overloads must
  // surface them too; an unrelated `bar` must not be added.
  EntityModel model;
  EntityMacro m;
  m.name = "PUBLIC";
  m.tokens = {"detail", "::", "foo", "(", ")"};
  model.macros.push_back(m);
  model.items.push_back({EntityItem::kFunction, "foo", {"detail"}});
  model.items.push_back({EntityItem::kFunction, "foo", {"detail"}});
  model.items.push_back({EntityItem::kFunction, "bar", {"detail"}});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  expand_overloads(r, model);
  bool has_foo = false;
  for (auto &f : r.reachable_fqns)
    if (f == "detail::foo") has_foo = true;
  EXPECT_TRUE(has_foo);
  bool has_bar = false;
  for (auto &f : r.reachable_fqns)
    if (f == "detail::bar") has_bar = true;
  EXPECT_FALSE(has_bar)
      << "an unrelated sibling function must not be surfaced";
}

TEST(MacroAnalysis, ExpandTransitiveTypesAddsReturnType) {
  // `detail::make` is reachable via a public macro; its return type is an
  // internal class. expand_transitive_types must surface the return type.
  EntityModel model;
  EntityMacro m;
  m.name = "PUBLIC";
  m.tokens = {"detail", "::", "make", "(", ")"};
  model.macros.push_back(m);
  model.items.push_back({EntityItem::kFunction, "make", {"detail"},
                         "", "", true, false, {"detail::Factory"}});
  model.items.push_back({EntityItem::kClass, "Factory", {"detail"}});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  expand_transitive_types(r, model);
  bool has = false;
  for (auto &f : r.reachable_fqns)
    if (f == "detail::Factory") has = true;
  EXPECT_TRUE(has)
      << "an internal return type of a reachable function must be surfaced";
}

TEST(MacroAnalysis, ExpandTransitiveTypesReachesMethodReturnTypes) {
  // A reachable class's public member function return type is an internal
  // class; expand_transitive_types must surface it.
  EntityModel model;
  EntityMacro m;
  m.name = "PUBLIC";
  m.tokens = {"detail", "::", "Holder"};
  model.macros.push_back(m);
  model.items.push_back({EntityItem::kClass, "Holder", {"detail"},
                         "", "", true, false, {"detail::Factory"}});
  model.items.push_back({EntityItem::kClass, "Factory", {"detail"}});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  expand_transitive_types(r, model);
  bool has = false;
  for (auto &f : r.reachable_fqns)
    if (f == "detail::Factory") has = true;
  EXPECT_TRUE(has)
      << "an internal return type of a reachable class's member must be "
         "surfaced";
}

TEST(MacroAnalysis, ExpandTransitiveTypesTerminatesOnCycles) {
  // `detail::A` references `detail::B` and vice versa. expand_transitive_types
  // must terminate (no infinite loop) and add both.
  EntityModel model;
  EntityMacro m;
  m.name = "PUBLIC";
  m.tokens = {"detail", "::", "A"};
  model.macros.push_back(m);
  model.items.push_back({EntityItem::kClass, "A", {"detail"},
                         "", "", true, false, {"detail::B"}});
  model.items.push_back({EntityItem::kClass, "B", {"detail"},
                         "", "", true, false, {"detail::A"}});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  expand_transitive_types(r, model);
  bool has_a = false, has_b = false;
  for (auto &f : r.reachable_fqns) {
    if (f == "detail::A") has_a = true;
    if (f == "detail::B") has_b = true;
  }
  EXPECT_TRUE(has_a);
  EXPECT_TRUE(has_b)
      << "a cyclic type reference must be closed without infinite looping";
}

TEST(MacroAnalysis, ExpandTransitiveTypesThroughPublicIntermediates) {
  // A reachable internal entity references a public intermediate whose type
  // signature mentions another internal entity; the expansion must walk the
  // public intermediate and surface the leaf internal type.
  EntityModel model;
  EntityMacro m;
  m.name = "PUBLIC";
  m.tokens = {"detail", "::", "start"};
  model.macros.push_back(m);
  model.items.push_back({EntityItem::kFunction, "start", {"detail"},
                         "", "", true, false, {"pub::Mid"}});
  model.items.push_back({EntityItem::kClass, "Mid", {"pub"},
                         "", "", true, false, {"detail::Leaf"}});
  model.items.push_back({EntityItem::kClass, "Leaf", {"detail"}});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  expand_transitive_types(r, model);
  bool has = false;
  for (auto &f : r.reachable_fqns)
    if (f == "detail::Leaf") has = true;
  EXPECT_TRUE(has)
      << "transitive type reachability must walk public intermediates";
}

TEST(MacroAnalysis, ManualReachableFqnDoesNotDisableMacroAnalysis) {
  // Passing an explicit `--reachable-fqn` must NOT disable macro reachability
  // analysis: internal entities referenced by public macros are detected
  // automatically and must still be exported/treated as normal modules even
  // when the user also lists some FQNs manually.
  auto a = data_path("mreach_lib/a.h");
  auto b = data_path("mreach_lib/internal/b.h");
  std::vector<std::string> headers = {b, a};
  std::vector<std::string> extra_args = {"-I", gDataDir};

  auto mm = modulizer::compute_macro_modules(
      headers, "mreach_lib", extra_args, {"mreach_lib::internal::helper"});
  bool b_module_in = false;
  for (auto &m : mm.macro_modules)
    if (m == "mreach_lib.internal.b") b_module_in = true;
  ASSERT_TRUE(b_module_in)
      << "a module reached via a public macro must still be marked as a "
         "normal module when a manual --reachable-fqn is also provided";
  bool a_module_in = false;
  for (auto &m : mm.macro_modules)
    if (m == "mreach_lib.a") a_module_in = true;
  EXPECT_TRUE(a_module_in)
      << "the module owning the manually listed FQN must also be marked";
  bool found_b_entity = false;
  for (auto &f : mm.macro_reachable)
    if (f == "mreach_lib::internal::helper_impl") found_b_entity = true;
  EXPECT_TRUE(found_b_entity)
      << "macro-reachable entities must survive alongside manual FQNs";
}

TEST(MacroAnalysis, InternalModuleReachableViaMacroInOtherFile) {
  auto a_internal = data_path("test_lib/a_internal.h");
  auto a = data_path("test_lib/a.h");
  std::vector<std::string> headers = {a_internal, a};
  std::vector<std::string> extra_args = {"-I", gDataDir};

  auto mm = modulizer::compute_macro_modules(headers, "test_lib", extra_args);
  bool in_modules = false;
  for (auto &m : mm.macro_modules)
    if (m == "test_lib.a_internal") in_modules = true;
  ASSERT_TRUE(in_modules)
      << "internal module reached via macro in another file must be "
         "treated as a normal module";

  std::map<std::string, std::string> include_map = {
    {"test_lib/a_internal.h", "test_lib.a_internal"}
  };
  auto r = rewrite_header(a, "test_lib.a", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = true, .extra_args = extra_args, .no_internal_filter = false, .import_std = false, .macro_modules = mm.macro_modules});
  EXPECT_NE(r.cc_content.find("export import test_lib.a_internal;"),
            std::string::npos)
      << "rewritten header must re-export the internal module reached via macro";
}

TEST(SharedHeaderModels, PerFileExtractionMatchesTheCombinedOne) {
  // Several --full analyses want the same thing from the library headers:
  // which entities each declares and completes, which macros each defines.
  // Each used to parse the whole header set for itself. The per-file
  // extraction is the primitive they now share, and merging it must reproduce
  // exactly what the combined extraction produced.
  std::vector<std::string> headers = {data_path("fwd_lib/a.h"),
                                      data_path("fwd_lib/b.h"),
                                      data_path("mreach_lib/a.h")};
  std::vector<std::string> extra_args = {"-I", gDataDir};

  auto combined = analyze_files_with_flags(headers, extra_args);
  auto merged = merge_entity_models(analyze_files_per_file(headers, extra_args));

  ASSERT_EQ(merged.items.size(), combined.items.size());
  ASSERT_EQ(merged.macros.size(), combined.macros.size());
  for (std::size_t i = 0; i < merged.items.size(); ++i) {
    EXPECT_EQ(fqn_of(merged.items[i].ns_path, merged.items[i].name),
              fqn_of(combined.items[i].ns_path, combined.items[i].name));
    EXPECT_EQ(merged.items[i].complete, combined.items[i].complete);
  }
}

TEST(SharedHeaderModels, PrecomputedModelsGiveIdenticalAnalyses) {
  // Handing those models to the analyses that would otherwise re-extract them
  // must not change a single answer.
  std::vector<std::string> headers = {data_path("fwd_lib/a.h"),
                                      data_path("fwd_lib/b.h"),
                                      data_path("mreach_lib/a.h")};
  std::vector<std::string> extra_args = {"-I", gDataDir};
  auto models = analyze_files_per_file(headers, extra_args);

  EXPECT_EQ(cross_module_fwd_declared_fqns(headers, extra_args, &models),
            cross_module_fwd_declared_fqns(headers, extra_args))
      << "forward-declaration analysis must not depend on who parsed";

  auto shared = cross_module_template_body_referenced_fqns(headers, extra_args,
                                                           &models);
  auto own = cross_module_template_body_referenced_fqns(headers, extra_args);
  EXPECT_EQ(shared.fwd_declared, own.fwd_declared);
  EXPECT_EQ(shared.aliases, own.aliases);

  auto mm_shared = modulizer::compute_macro_modules(headers, "fwd_lib",
                                                    extra_args, {},
                                                    &models);
  auto mm_own = modulizer::compute_macro_modules(headers, "fwd_lib", extra_args);
  EXPECT_EQ(mm_shared.macro_reachable, mm_own.macro_reachable);
  EXPECT_EQ(mm_shared.macro_modules, mm_own.macro_modules);
}

TEST(MacroAnalysis, KeepsGuardOfConditionalNestedInAnIncludeGuard) {
  // A macro-only conditional is emitted verbatim so the right branch expands on
  // the actual platform. Nested conditionals are left to the enclosing block,
  // which carries them along — but only when that block is itself emitted.
  //
  // A header wrapped in an `#ifndef` include guard looks macro-only at the
  // moment the nested block closes, because the code that disqualifies it comes
  // further down the file. Suppressing the nested block on that basis lost its
  // guard entirely: the enclosing block was never emitted, and the macros file
  // ended up with whichever branch the conversion host happened to take.
  std::string src =
      "#ifndef LIB_DEFS_H_\n"
      "#define LIB_DEFS_H_\n"
      "\n"
      "#ifdef __has_include\n"
      "#define LIB_HAS_INCLUDE __has_include\n"
      "#else\n"
      "#define LIB_HAS_INCLUDE(...) 0\n"
      "#endif\n"
      "\n"
      "namespace lib { inline int use() { return 1; } }\n"
      "\n"
      "#endif  // LIB_DEFS_H_\n";

  std::vector<MacroRec> macros;
  extract_textual_macros(src, macros);

  auto block = std::ranges::find_if(macros, [](const MacroRec &m) {
    return m.name.empty();
  });
  ASSERT_NE(block, macros.end())
      << "the macro-only conditional must be emitted verbatim";
  EXPECT_NE(block->body.find("#ifdef __has_include"), std::string::npos)
      << "its guard must be kept, not just the branch taken here";
  EXPECT_NE(block->body.find("#define LIB_HAS_INCLUDE __has_include"),
            std::string::npos);
  EXPECT_NE(block->body.find("#define LIB_HAS_INCLUDE(...) 0"),
            std::string::npos)
      << "both branches must survive so the header ports to a compiler that "
         "takes the other one";
  EXPECT_EQ(block->body.find("namespace lib"), std::string::npos)
      << "the enclosing include guard mixes in code and must not be emitted";
}
