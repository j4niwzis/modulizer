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
// WrapperGen suite
TEST(WrapperGen, ExportsInternalReachableViaMacro) {
  auto model = analyze_files({data_path("lib_a.h"), data_path("lib_macro.h")});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  auto out = generate_wrapper_cc("lib_test",
                                  {"lib_a.h", "lib_macro.h"},
                                  model,
                                  kDefaultInternalFilter,
                                  r.reachable_fqns);
  EXPECT_NE(out.find("using ::lib_test::internal::foo"), std::string::npos);
}

TEST(WrapperGen, GeneratesCorrectStructure) {
  auto model = analyze_file(data_path("macro_test.h"));
  auto out = generate_wrapper_cc("test_lib", {"macro_test.h"}, model);
  EXPECT_NE(out.find("export module test_lib;"), std::string::npos);
  EXPECT_NE(out.find("using ::test_lib::Foo"), std::string::npos);
}

TEST(WrapperGen, FiltersInternalNamespace) {
  EntityModel model;
  model.items.push_back({EntityItem::kClass, "Foo",
                         {"test_lib", "detail"}});
  model.items.push_back({EntityItem::kClass, "Public",
                         {"test_lib"}});
  auto out = generate_wrapper_cc("test", {"h.h"}, model);
  EXPECT_NE(out.find("using ::test_lib::Public"), std::string::npos);
  EXPECT_EQ(out.find("Helper"), std::string::npos);
}

TEST(WrapperGen, IncludesReachableInternals) {
  EntityModel model;
  model.items.push_back({EntityItem::kClass, "Exposed",
                         {"test_lib", "detail"}});
  model.items.push_back({EntityItem::kClass, "Hidden",
                         {"test_lib", "detail"}});
  std::vector<std::string> reachable = {"test_lib::detail::Exposed"};
  auto out = generate_wrapper_cc("test", {"h.h"}, model,
                                 kDefaultInternalFilter, reachable);
  EXPECT_NE(out.find("test_lib::detail::Exposed"), std::string::npos);
  EXPECT_EQ(out.find("test_lib::detail::Hidden"), std::string::npos);
}

TEST(WrapperGen, GeneratesCompanionHeader) {
  EntityModel model;
  model.macros.push_back({"FOO", "#define FOO 42", false, {}, {"42"}});
  model.macros.push_back({"BAR", "#define BAR(x) x + 1", true, {"x"},
                          {"x", "+", "1"}});
  auto out = generate_companion_h(model);
  EXPECT_NE(out.find("#define FOO 42\n"), std::string::npos)
      << "object-like macros are emitted as real #define directives";
  EXPECT_NE(out.find("#define BAR(x) x + 1\n"), std::string::npos)
      << "function-like macros keep their parameter list";
}

TEST(WrapperGen, CompanionHeaderFromRealHeaderHasDefineDirectives) {
  // The companion header must reproduce the header's `#define` lines verbatim
  // (with the `#define` keyword), not a name-then-tokens fragment.
  auto model = analyze_file(data_path("macro_test.h"));
  auto out = generate_companion_h(model);
  EXPECT_NE(out.find("#define PUBLIC_HELPER "), std::string::npos)
      << "an object-like macro must be emitted as a #define";
  EXPECT_NE(out.find("#define CONVERT(x) "), std::string::npos)
      << "a function-like macro must keep its parameter list";
}

TEST(WrapperGen, CompanionHeaderFiltersGuardsAndUndefs) {
  // In wrapper mode the companion header must NOT contain include guards (each
  // header is parsed as the main file, so clang misses them) nor macros that
  // any header in the batch `#undef`s — at the end of their own file or in
  // ANOTHER header of the batch. A_STABLE is defined in a.h but `#undef`'d in
  // b.h (cross-header); A_TMP is defined then `#undef`'d at the end of a.h.
  clang::tooling::FixedCompilationDatabase db(
      ".", {"-x", "c++", "-std=c++20", "-I", gDataDir});
  auto model = analyze_headers_with_cdb(
      db, {data_path("wrapper_mac_lib/a.h"), data_path("wrapper_mac_lib/b.h")},
      /*wrapper_mode=*/true);
  auto out = generate_companion_h(model);
  EXPECT_NE(out.find("#define B_KEEP "), std::string::npos)
      << "a macro that no header undefines is kept";
  EXPECT_EQ(out.find("WRAPPER_MAC_A_H"), std::string::npos)
      << "the a.h include guard must be filtered out";
  EXPECT_EQ(out.find("WRAPPER_MAC_B_H"), std::string::npos)
      << "the b.h include guard must be filtered out";
  EXPECT_EQ(out.find("A_TMP"), std::string::npos)
      << "a macro #undef'd at the end of its own file must be filtered out";
  EXPECT_EQ(out.find("A_STABLE"), std::string::npos)
      << "a macro #undef'd in another header of the batch must be filtered out";
}

TEST(WrapperGen, EmitsGuardsForGuardedEntities) {
  auto model = analyze_file(data_path("guarded.h"));
  auto out = generate_wrapper_cc("test_lib", {"guarded.h"}, model);
  // AlwaysAvailable has no guard
  EXPECT_NE(out.find("using ::test_lib::AlwaysAvailable"), std::string::npos);
  // WindowsOnly is guarded by #ifdef HAS_WINDOWS
  EXPECT_NE(out.find("#ifdef HAS_WINDOWS"), std::string::npos);
  EXPECT_NE(out.find("using ::test_lib::WindowsOnly"), std::string::npos);
  // UnixFallback is in #else, guarded by #ifndef HAS_WINDOWS
  EXPECT_NE(out.find("#ifndef HAS_WINDOWS"), std::string::npos);
  EXPECT_NE(out.find("using ::test_lib::UnixFallback"), std::string::npos);
  // feature_fn is guarded by #ifndef SKIP_FEATURE
  EXPECT_NE(out.find("#ifndef SKIP_FEATURE"), std::string::npos);
  EXPECT_NE(out.find("using ::test_lib::feature_fn"), std::string::npos);
  // Legacy is guarded by #ifndef NO_LEGACY
  EXPECT_NE(out.find("#ifndef NO_LEGACY"), std::string::npos);
  EXPECT_NE(out.find("using ::test_lib::Legacy"), std::string::npos);
}

TEST(WrapperGen, SkipsDependentTemplateEntityNames) {
  // Entity names that contain template-parameter syntax (e.g. `Foo<T>` or the
  // corrupted `Foo<type-parameter-1-0>` produced for dependent names) are not
  // real entities: the wrapper must not re-export them (they would emit an
  // invalid `using` declaration). Primary templates are exported by default, so
  // only the usable `bar` must appear.
  EntityModel model;
  model.items.push_back({EntityItem::kFunction, "Foo<T>", {"wraplib"}});
  model.items.push_back(
      {EntityItem::kFunction, "operator Foo<type-parameter-1-0>",
       {"wraplib", "internal"}});
  model.items.push_back({EntityItem::kFunction, "bar", {"wraplib"}});
  auto out = generate_import_wrapper_cc(
      "wraplib", "wraplib.umbrella", model, kDefaultInternalFilter,
      /*reachable_internal=*/{"wraplib::internal::operator Foo<type-parameter-1-0>"});
  EXPECT_NE(out.find("using ::wraplib::bar;"), std::string::npos);
  EXPECT_EQ(out.find("Foo<T>"), std::string::npos)
      << "a dependent template name must not be re-exported";
  EXPECT_EQ(out.find("type-parameter"), std::string::npos)
      << "a corrupted dependent name must not be re-exported";
}

TEST(WrapperGen, ImportWrapperImportsAllLibraryModules) {
  // The wrapper re-exports entities that live in ANY of the library's interface
  // modules (e.g. a reporter class lives in the
  // spi module, not the umbrella). It must import every library module so
  // those `using ::ns::Entity;` re-exports resolve.
  EntityModel model;
  model.items.push_back({EntityItem::kClass, "ScopedReporter", {"wraplib"}});
  auto out = generate_import_wrapper_cc(
      "wraplib", "wraplib.umbrella", model, kDefaultInternalFilter,
      /*reachable_internal=*/{},
      /*import_std=*/false,
      /*import_modules=*/{"wraplib.umbrella", "wraplib.spi", "wraplib.matchers"});
  EXPECT_NE(out.find("import wraplib.umbrella;\n"), std::string::npos);
  EXPECT_NE(out.find("import wraplib.spi;\n"), std::string::npos)
      << "the wrapper must import every library interface module";
  EXPECT_NE(out.find("import wraplib.matchers;\n"), std::string::npos);
}

TEST(WrapperGen, ExportsGlobalNamespaceEntities) {
  // An entity in the global namespace (e.g. a test-runner entry point, formerly
  // a macro) is re-exported outside any namespace block, so its `using`
  // declaration must carry `export` — otherwise consumers importing the wrapper
  // cannot see it.
  EntityModel model;
  model.items.push_back({EntityItem::kFunction, "RunAll", {}});
  model.items.push_back({EntityItem::kClass, "Public", {"wraplib"}});
  auto out = generate_import_wrapper_cc(
      "wraplib", "wraplib.umbrella", model, kDefaultInternalFilter);
  EXPECT_NE(out.find("export using ::RunAll;\n"), std::string::npos)
      << "a global-namespace re-export must be exported";
  EXPECT_NE(out.find("using ::wraplib::Public;"), std::string::npos)
      << "namespace-scope re-exports stay plain `using` inside the exported "
         "namespace";
}

TEST(WrapperGen, InternalForwardDeclaredFunctionKeepsInternalNamespace) {
  // A forward-declared function inside an internal namespace must keep its
  // internal namespace in the model, so the wrapper re-exports it under the
  // correct `export namespace internal` (matching the umbrella's export) rather
  // than as a public `wraplib::helper_func`.
  auto model = analyze_file(data_path("wraplib/producer.h"));
  bool found = false;
  for (auto &it : model.items) {
    if (it.name == "helper_func") {
      found = true;
      EXPECT_EQ(it.ns_path, (std::vector<std::string>{"wraplib", "internal"}))
          << "a forward-declared function in an internal namespace must keep "
             "the internal namespace in the model";
    }
  }
  EXPECT_TRUE(found);
}

TEST(WrapperGen, CdbModelKeepsInternalNamespaceForForwardDecls) {
  // The wrapper model is built via analyze_headers_with_cdb (a single ClangTool
  // over the headers); a forward-declared internal function must keep the
  // internal namespace there too, otherwise the wrapper re-exports it as
  // public (`using ::wraplib::helper_func;`) which fails to resolve against the
  // umbrella.
  clang::tooling::FixedCompilationDatabase db(
      ".", {"-x", "c++", "-std=c++20", "-I", gDataDir});
  auto model = analyze_headers_with_cdb(db, {data_path("wraplib/producer.h")});
  bool found = false;
  for (auto &it : model.items) {
    if (it.name == "helper_func") {
      found = true;
      EXPECT_EQ(it.ns_path, (std::vector<std::string>{"wraplib", "internal"}))
          << "the cdb-based model must keep the internal namespace for a "
             "forward-declared internal function";
    }
  }
  EXPECT_TRUE(found);
}

TEST(WrapperGen, ReexportsUsingDeclaration) {
  // A `using internal::Spec;` (inside a namespace) hoists an
  // internal class into the public namespace. The wrapper must re-export that
  // name (`using ::ns::Spec;`) so consumers get the
  // alias their declaration macro expansion references.
  clang::tooling::FixedCompilationDatabase db(
      ".", {"-x", "c++", "-std=c++20", "-I", gDataDir});
  auto model = analyze_headers_with_cdb(db, {data_path("wraplib/producer.h")});
  auto out = generate_import_wrapper_cc(
      "wraplib", "wraplib.umbrella", model, kDefaultInternalFilter);
  EXPECT_NE(out.find("using ::wraplib::adl_guard::Baz;"), std::string::npos);
}

TEST(WrapperGen, ReexportsUsingDirective) {
  // A namespace-scope `using namespace adl_guard;` directive must be re-exported
  // by the wrapper so consumers resolve a name (which lives in
  // `adl_guard`) through it — not just the qualified adl_guard names.
  clang::tooling::FixedCompilationDatabase db(
      ".", {"-x", "c++", "-std=c++20", "-I", gDataDir});
  auto model = analyze_headers_with_cdb(db, {data_path("wraplib/producer.h")});
  auto out = generate_import_wrapper_cc(
      "wraplib", "wraplib.umbrella", model, kDefaultInternalFilter);
  EXPECT_NE(out.find("using namespace adl_guard;"), std::string::npos)
      << "a using-namespace directive must be re-exported by the wrapper";
}

TEST(WrapperGen, SkipsClassMemberAndFriendEntities) {
  // Class-scope declarations (member typedefs, friend-declared classes) are not
  // namespace-scope entities: the wrapper must not re-export them as
  // `using ::ns::Name;` (there is no such public name to resolve against).
  clang::tooling::FixedCompilationDatabase db(
      ".", {"-x", "c++", "-std=c++20", "-I", gDataDir});
  auto model = analyze_headers_with_cdb(db, {data_path("friendlib/friendlib.h")});
  auto out = generate_import_wrapper_cc(
      "friendlib", "friendlib.umbrella", model, kDefaultInternalFilter);
  // The public class itself is re-exported.
  EXPECT_NE(out.find("using ::friendlib::Public;"), std::string::npos);
  // The class-scope typedef and the friend-declared template are not.
  EXPECT_EQ(out.find("friendlib::size_t"), std::string::npos)
      << "a class-scope member typedef must not be re-exported";
  EXPECT_EQ(out.find("friendlib::internal::Helper"), std::string::npos)
      << "a friend-declared class must not be misattributed to the wrapper";
}

TEST(WrapperGen, WrapsTheBodyInExternCxxAndSeparatesCLinkage) {
  // Everything a header wrapper re-exports belongs to the global module — it
  // is declared in a header included in the global module fragment — so the
  // whole body is one `export extern "C++"` block. That is also what makes
  // entities with internal linkage re-exportable at all: a plain exported
  // using declaration naming a `const` variable at namespace scope is rejected
  // ("using declaration referring to 'X' with internal linkage cannot be
  // exported"), and `export` has to sit outside the linkage block, not inside
  // it. Entities with C language linkage cannot be in that block and get their
  // own `export extern "C"` one.
  clang::tooling::FixedCompilationDatabase db(
      ".", {"-x", "c++", "-std=c++20", "-I", gDataDir});
  auto model = analyze_headers_with_cdb(db, {data_path("linkage_lib/api.h")});
  auto out = generate_wrapper_cc("linkage_lib", {"linkage_lib/api.h"}, model);

  EXPECT_NE(out.find("export extern \"C++\" {"), std::string::npos)
      << "the C++ body must be one exported linkage block";
  EXPECT_NE(out.find("export extern \"C\" {"), std::string::npos)
      << "C-linkage entities need a block of their own";

  auto cxx_at = out.find("export extern \"C++\" {");
  auto c_at = out.find("export extern \"C\" {");
  auto pos = [&](std::string_view needle) { return out.find(needle); };

  // The internal-linkage constants are re-exported, inside the C++ block.
  ASSERT_NE(pos("using ::linkage_lib::kToggle;"), std::string::npos);
  EXPECT_GT(pos("using ::linkage_lib::kToggle;"), cxx_at);
  EXPECT_LT(pos("using ::linkage_lib::kToggle;"), c_at);
  EXPECT_NE(pos("using ::linkage_lib::kLimit;"), std::string::npos);
  EXPECT_NE(pos("using ::linkage_lib::compute;"), std::string::npos);
  EXPECT_NE(pos("using ::linkage_lib::Widget;"), std::string::npos);

  // The C entry point goes after the C block opens.
  ASSERT_NE(pos("using ::linkage_lib_c_entry;"), std::string::npos);
  EXPECT_GT(pos("using ::linkage_lib_c_entry;"), c_at)
      << "a C-linkage function must not sit in the extern \"C++\" block";
}
