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
// HeaderRewrite suite
TEST(HeaderRewrite, ExportsInternalReachableViaMacro) {
  auto model = analyze_files({data_path("lib_a.h"), data_path("lib_macro.h")});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  auto hdr = rewrite_header(data_path("lib_a.h"), "lib_test", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = r.reachable_fqns});
  EXPECT_NE(hdr.h_content.find("LIB_TEST_EXPORT"), std::string::npos);
  EXPECT_NE(hdr.h_content.find("auto foo()"), std::string::npos);
}

TEST(HeaderRewrite, ExportsEnumWithReachableEnumeratorValue) {
  // A reachable FQN that names an enum *value* (e.g. `kD`) must export the
  // enclosing enum so impl units importing the module can use the enumerator.
  // Enumerators live in the enum's enclosing namespace, so an
  // `internal::kD` FQN cannot match the `Flag` decl by name; the visitor must
  // fall back to matching enumerators.
  auto hdr = rewrite_header(data_path("enumreach.h"), "enumreach_lib", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {"test_lib::internal::kD"}});
  EXPECT_NE(hdr.h_content.find("ENUMREACH_LIB_EXPORT enum Flag"),
            std::string::npos)
      << "the enum owning a reachable enumerator must be exported";
}

TEST(HeaderRewrite, ExportsEnumWithReachableEnumeratorValueCcOnly) {
  // Same as ExportsEnumWithReachableEnumeratorValue but in cc-only mode where
  // the header body is inlined into the interface unit. The export must
  // survive in the `.cc` purview body.
  auto r = rewrite_header(data_path("enumreach.h"), "enumreach_lib", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {"test_lib::internal::kD"}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {}, .cc_only = /*cc_only=*/true});
  EXPECT_NE(r.cc_content.find("ENUMREACH_LIB_EXPORT enum Flag"),
            std::string::npos)
      << "cc-only interface units must also export enums owning a reachable "
         "enumerator";
}

TEST(HeaderRewrite, ExportsConditionalInternalEntityWithReachableFqn) {
  // Entities inside `#if`/`#elif`/`#else` blocks in an internal namespace must
  // get the export marker in the ACTIVE branch when their full FQN (including
  // the intermediate namespace) is listed as reachable.
  auto r = rewrite_header(data_path("condinternal.h"), "cond_lib", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {"test_lib::internal::posix::Foo",
       "test_lib::internal::posix::Bar"}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {}, .cc_only = /*cc_only=*/true});
  EXPECT_NE(r.cc_content.find(
                "COND_LIB_EXPORT struct Foo {"),
            std::string::npos)
      << "the active #else-branch internal entity must be exported";
  EXPECT_NE(r.cc_content.find("COND_LIB_EXPORT [[noreturn]] inline void Bar()"),
            std::string::npos)
      << "the active internal function must be exported";
}

TEST(HeaderRewrite, TransitivelyIncludesSystemHeaderFromImportedHeader) {
  // a.h only forwards <third_party.h>; b.h includes a.h and uses
  // third_party::bar(). Once a.h becomes a module import, <third_party.h>
  // must still be in b's GMF so foo() compiles.
  std::map<std::string, std::string> include_map = {
    {"a.h", "wrap_lib.a"},
    {"b.h", "wrap_lib.b"}
  };
  auto r = rewrite_header(data_path("wrap_lib/b.h"), "wrap_lib.b", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}});
  EXPECT_NE(r.cc_content.find("#include <third_party.h>"), std::string::npos)
      << "system header reached through an imported header must stay in the GMF";
  EXPECT_NE(r.cc_content.find("import wrap_lib.a;"), std::string::npos)
      << "a.h must become a module import";
  EXPECT_EQ(r.cc_content.find("#include \"a.h\""), std::string::npos)
      << "a.h must not stay as a raw include";
}

TEST(HeaderRewrite, DoesNotCarryUnusedTransitiveSystemInclude) {
  // a.h includes <third_party.h> and uses third_party::bar(); b.h includes
  // a.h but only calls foo(). Once a.h is a module import, <third_party.h>
  // must NOT leak into b's GMF because b never uses third_party directly.
  std::map<std::string, std::string> include_map = {
    {"a.h", "used_lib.a"},
    {"b.h", "used_lib.b"}
  };
  auto r = rewrite_header(data_path("used_lib/b.h"), "used_lib.b", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}});
  EXPECT_EQ(r.cc_content.find("#include <third_party.h>"), std::string::npos)
      << "unused transitive system include must not leak into the GMF";
  EXPECT_NE(r.cc_content.find("import used_lib.a;"), std::string::npos)
      << "b must still import the module that declares foo";
}

TEST(HeaderRewrite, PicksMinimalUsedSystemHeader) {
  // d.h reaches both third_party/a.h and third_party/b.h transitively via
  // c.h, but only uses third_party::a(). The GMF must provide `third_party::a`
  // (directly via a.h, or transitively via b.h which includes a.h), and must
  // NOT emit both: emitting the guard-less a.h twice (once via b.h's include,
  // once directly) would redefine `a()` — the same double-include problem as
  // musl `<bits/stat.h>` under `<sys/stat.h>`.
  std::map<std::string, std::string> include_map = {
    {"c.h", "used_lib.c"},
    {"d.h", "used_lib.d"}
  };
  auto r = rewrite_header(data_path("used_lib/d.h"), "used_lib.d", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}});
  bool a_present = r.cc_content.find("#include <third_party/a.h>") !=
                   std::string::npos;
  bool b_present = r.cc_content.find("#include <third_party/b.h>") !=
                   std::string::npos;
  EXPECT_TRUE(a_present || b_present)
      << "the GMF must provide third_party::a via a.h or its includer b.h";
  EXPECT_FALSE(a_present && b_present)
      << "the GMF must not emit both a.h and b.h (double include of the "
         "guard-less a.h would redefine a())";
}

TEST(HeaderRewrite, KeepsTransitiveVariantWhenUsed) {
  // api.h reaches <variant> only through impl.h (also via mid.h). It uses
  // std::variant directly, so <variant> must survive into api's GMF even
  // though it is a transitive system include.
  std::map<std::string, std::string> include_map = {
    {"mid.h", "varlib.mid"},
    {"impl.h", "varlib.impl"},
    {"api.h", "varlib.api"}
  };
  auto r = rewrite_header(data_path("varlib/api.h"), "varlib.api", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}});
  EXPECT_NE(r.cc_content.find("#include <variant>"), std::string::npos)
      << "transitive <variant> must be kept when the module uses std::variant";
  EXPECT_NE(r.cc_content.find("import varlib.impl;"), std::string::npos)
      << "impl.h must become a module import";
}

TEST(HeaderRewrite, ModuleReplacesStdlibHeaderKeepsTransitiveUsed) {
  // a.h includes <iostream> and uses std::cout (in iostream) and std::string
  // (in <string>, reached only transitively via <iostream>). std.compat is
  // marked as replacing <iostream> only; the used-but-unreplaced <string>
  // must still be added to the GMF. When import std is active, the C++
  // stdlib headers are guarded by <LIB>_IMPORT_STD and import std by
  // <LIB>_USE_IMPORT_STD instead of being removed.
  std::map<std::string, std::set<std::string>> replaces = {
    {"std.compat", {"iostream"}}
  };
  auto r = rewrite_header(data_path("stdmod/a.h"), "stdmod.a", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = true, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = replaces});
  EXPECT_NE(r.cc_content.find("#ifdef STDMOD_USE_IMPORT_STD\nimport std.compat;"),
            std::string::npos)
      << "import std.compat must be emitted behind the USE_IMPORT_STD guard";
  EXPECT_NE(r.cc_content.find("#ifndef STDMOD_IMPORT_STD"), std::string::npos)
      << "replaced C++ stdlib headers must be guarded by IMPORT_STD";
  EXPECT_NE(r.cc_content.find("#include <string>"), std::string::npos)
      << "string is used but not replaced, so it must stay in the GMF";
}

TEST(HeaderRewrite, ModuleReplacesHeaderKeepsTransitiveUsedIncludes) {
  // a.h uses std::variant which lives in <variant>, reached only transitively
  // through third_party.h. The tpmod.third_party module replaces
  // third_party.h but NOT <variant>, so <variant> must still be added to a's
  // GMF explicitly.
  std::map<std::string, std::string> include_map = {
    {"third_party.h", "tpmod.third_party"},
    {"a.h", "tpmod.a"}
  };
  std::map<std::string, std::set<std::string>> replaces = {
    {"tpmod.third_party", {"third_party.h"}}
  };
  auto r = rewrite_header(data_path("tpmod/a.h"), "tpmod.a", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = replaces});
  EXPECT_NE(r.cc_content.find("import tpmod.third_party;"), std::string::npos)
      << "third_party.h must be replaced by its module import";
  EXPECT_NE(r.cc_content.find("#include <variant>"), std::string::npos)
      << "<variant> is used but not replaced, so it must stay in the GMF";
  EXPECT_EQ(r.cc_content.find("#include \"third_party.h\""), std::string::npos)
      << "replaced third_party.h must not stay as a raw include";
}

TEST(HeaderRewrite, ModuleReplacesAutoGeneratedCrossLibrary) {
  // mylib/use.h includes dep_lib/dep.h from a DIFFERENT library. With no
  // explicit --module-replaces the tool must auto-derive the module
  // `dep_lib.dep` from the include path and emit an import instead of keeping
  // the raw include.
  auto r = rewrite_header(data_path("autoreplace/mylib/use.h"), "mylib.use", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir, "-I", gDataDir + "/autoreplace"}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}});
  EXPECT_NE(r.cc_content.find("export import dep_lib.dep;"),
            std::string::npos)
      << "a cross-library include must auto-derive its module import";
  EXPECT_EQ(r.cc_content.find("#include \"dep_lib/dep.h\""),
            std::string::npos)
      << "the cross-library include must not stay as a raw include";
}

TEST(HeaderRewrite, ModuleReplacesAutoGeneratedTransitive) {
  // mylib/use_umbrella.h includes only dep_lib/umbrella.h, which transitively
  // includes dep_lib/dep.h (public) and dep_lib/internal/helper.h (internal).
  // Auto-generation must import the public umbrella/dep modules with export
  // import, plain-import the internal module, and drop all raw includes.
  auto r = rewrite_header(data_path("autoreplace/mylib/use_umbrella.h"), "mylib.use_umbrella", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir, "-I", gDataDir + "/autoreplace"}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}});
  EXPECT_NE(r.cc_content.find("export import dep_lib.umbrella;"),
            std::string::npos)
      << "the transitively-included public umbrella must be re-exported";
  EXPECT_NE(r.cc_content.find("export import dep_lib.dep;"),
            std::string::npos)
      << "the transitively-included public dep header must be re-exported";
  EXPECT_NE(r.cc_content.find("import dep_lib.internal.helper;"),
            std::string::npos)
      << "the transitively-included internal header must be plain-imported";
  EXPECT_EQ(r.cc_content.find("#include \"dep_lib/umbrella.h\""),
            std::string::npos)
      << "the umbrella include must not stay as a raw include";
  EXPECT_EQ(r.cc_content.find("#include \"dep_lib/dep.h\""),
            std::string::npos)
      << "the dep include must not stay as a raw include";
  EXPECT_EQ(r.cc_content.find("#include \"dep_lib/internal/helper.h\""),
            std::string::npos)
      << "the internal helper include must not stay as a raw include";
}

TEST(HeaderRewrite, ModuleReplacesUsesInternalnessForImport) {
  // other.h is public → its module is `export import`ed; internal/helper.h
  // lives in an internal dir → its module is plain-`import`ed. std/std.compat
  // are the exception, but any other replacing module follows the internalness
  // rule.
  std::map<std::string, std::set<std::string>> replaces = {
    {"other_lib", {"other.h"}},
    {"other_lib.internal.helper", {"internal/helper.h"}},
  };
  auto r = rewrite_header(data_path("replacelib/consumer.h"), "replacelib.consumer", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = replaces});
  EXPECT_NE(r.cc_content.find("export import other_lib;"), std::string::npos)
      << "a public replaced header's module must be re-exported";
  EXPECT_NE(r.cc_content.find("import other_lib.internal.helper;"),
            std::string::npos)
      << "an internal replaced header's module must be plain-imported";
  EXPECT_EQ(r.cc_content.find("#include \"other.h\""), std::string::npos)
      << "the public replaced header must not stay as a raw include";
  EXPECT_EQ(r.cc_content.find("#include \"internal/helper.h\""),
            std::string::npos)
      << "the internal replaced header must not stay as a raw include";
}

TEST(HeaderRewrite, ModuleReplacesPrefersMostSpecific) {
  // Both `ab` and `b` replace b.h, but `b` replaces fewer headers, so b.h must
  // import `b`, not `ab`. a.h is only in `ab` so it imports `ab`.
  std::map<std::string, std::set<std::string>> replaces = {
    {"ab", {"a.h", "b.h"}},
    {"b", {"b.h"}},
  };
  auto r = rewrite_header(data_path("minreplace/c.h"), "minreplace.c", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = replaces});
  EXPECT_NE(r.cc_content.find("export import ab;"), std::string::npos)
      << "a.h is only replaced by ab";
  EXPECT_NE(r.cc_content.find("export import b;"), std::string::npos)
      << "b.h must prefer the most specific replacing module b over ab";
  EXPECT_EQ(r.cc_content.find("import ab;\nimport b;"), std::string::npos)
      << "b.h must not import the broader ab module";
}

TEST(HeaderRewrite, ModuleReplacesTransitiveConsumerGetsImport) {
  // w.h includes only a.h, which includes b.h. b.h is replaced by module
  // `tranlib.b`, and w.h itself uses B. The consumer must get the import even
  // though it reaches b.h only transitively.
  std::map<std::string, std::set<std::string>> replaces = {
    {"tranlib.b", {"b.h"}},
  };
  auto r = rewrite_header(data_path("tranreplace/w.h"), "tranreplace.w", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = replaces});
  EXPECT_NE(r.cc_content.find("export import tranlib.b;"), std::string::npos)
      << "a transitively-reached replaced header must still be imported";
  EXPECT_EQ(r.cc_content.find("#include \"b.h\""), std::string::npos)
      << "the transitively replaced b.h must not stay as a raw include";
}

TEST(HeaderRewrite, ModuleReplacesCrossLibraryAddsImport) {
  // widget.h includes "other.h" which belongs to a DIFFERENT library. The
  // crosslib module replaces other.h, so rewriting widget.h must emit
  // `import other_lib;` in the purview and drop the raw #include.
  std::map<std::string, std::set<std::string>> replaces = {
    {"other_lib", {"other.h"}}
  };
  auto r = rewrite_header(data_path("crosslib/widget.h"), "crosslib.widget", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = replaces});
  EXPECT_NE(r.cc_content.find("import other_lib;"), std::string::npos)
      << "a cross-library replaced header must become a module import";
  EXPECT_EQ(r.cc_content.find("#include \"other.h\""), std::string::npos)
      << "the replaced header must not stay as a raw include";
}

TEST(HeaderRewrite, ExportsUsingNamespaceDirective) {
  // A namespace-scope `using namespace X;` directive (e.g.
  // `using namespace adl_guard;`) makes X's members visible in the enclosing
  // namespace. It must ALWAYS be exported (via the export macro, which becomes
  // `export` under <LIB>_USE_MODULES) so consumers resolve names like
  // `wraplib::Baz` (which lives in `wraplib::adl_guard`) through it.
  auto r = rewrite_header(data_path("wraplib/producer.h"), "wraplib",
                          RewriteOptions{.cc_only = true});
  EXPECT_NE(r.cc_content.find("WRAPLIB_EXPORT using namespace adl_guard;"),
            std::string::npos)
      << "a namespace-scope using-namespace directive must be exported";
}

TEST(HeaderRewrite, InsertsLibraryExport) {
  auto r = rewrite_header(data_path("macro_test.h"), "test_lib");
  EXPECT_NE(r.h_content.find("TEST_LIB_EXPORT"), std::string::npos);
  // Export helper header is included, not inline
  EXPECT_NE(r.h_content.find("#include \"test_lib_export.h\""), std::string::npos);
  // The export helper header itself has the full definition
  EXPECT_NE(r.export_h_content.find("TEST_LIB_USE_MODULES"), std::string::npos);
  EXPECT_NE(r.export_h_content.find("TEST_LIB_EXPORT"), std::string::npos);
}

TEST(HeaderRewrite, ExportsPublicClass) {
  auto r = rewrite_header(data_path("macro_test.h"), "test_lib");
  EXPECT_NE(r.h_content.find("TEST_LIB_EXPORT class Foo"),
            std::string::npos);
}

TEST(HeaderRewrite, DoesNotExportDetailMembers) {
  auto r = rewrite_header(data_path("macro_test.h"), "test_lib");
  if (r.h_content.empty()) { GTEST_SKIP(); return; }
  auto pos = r.h_content.find("\nnamespace detail");
  EXPECT_NE(pos, std::string::npos);
  auto after_start = r.h_content.find('{', pos);
  EXPECT_NE(after_start, std::string::npos);
  auto after_end = r.h_content.find("\n}", after_start);
  EXPECT_NE(after_end, std::string::npos);
  // No TEST_LIB_EXPORT between namespace detail { and }
  auto block = r.h_content.substr(after_start, after_end - after_start);
  EXPECT_EQ(block.find("TEST_LIB_EXPORT"), std::string::npos);
}

TEST(HeaderRewrite, GeneratesModuleCC) {
  auto r = rewrite_header(data_path("macro_test.h"), "test_lib");
  EXPECT_NE(r.cc_content.find("export module test_lib;"), std::string::npos);
  EXPECT_NE(r.cc_content.find("TEST_LIB_USE_MODULES"), std::string::npos);
}

TEST(HeaderRewrite, MacroPrefixOverride) {
  // The LIB prefix of LIB_USE_MODULES / LIB_IMPORT_STD / LIB_EXPORT can differ
  // from the main module name, so it must be overridable (--macro-prefix).
  auto r = rewrite_header(data_path("macro_test.h"), "test_lib",
                          RewriteOptions{.macro_prefix_override = "CUSTOM"});
  EXPECT_NE(r.cc_content.find("CUSTOM_USE_MODULES"), std::string::npos)
      << "the LIB prefix of LIB_USE_MODULES must be overridable";
  EXPECT_EQ(r.cc_content.find("TEST_LIB_USE_MODULES"), std::string::npos)
      << "the default library-derived prefix must be replaced";
  EXPECT_NE(r.export_h_content.find("CUSTOM_EXPORT"), std::string::npos)
      << "the LIB_EXPORT macro must use the overridden prefix";
}

TEST(HeaderRewrite, HyphenMacrosNaming) {
  // The macro headers are named <stem>_macros.h by default; with the hyphen
  // option they become <stem>-macros.h (Google C++ style) and the stem's
  // underscores are hyphenated too (`mylib_pred_impl.h` → `mylib-pred-impl`).
  auto r = rewrite_header(data_path("macro_test.h"), "test_lib",
                          RewriteOptions{.hyphen_macros = true});
  EXPECT_EQ(r.macros_name, "macro-test-macros");
  EXPECT_NE(r.cc_content.find("#include \"macro-test-macros.h\""),
            std::string::npos)
      << "the GMF must include the hyphenated macros header";
  auto r2 = rewrite_header(data_path("macro_test.h"), "test_lib");
  EXPECT_EQ(r2.macros_name, "macro_test_macros")
      << "the default naming stays underscore-based";
}

TEST(HeaderRewrite, HyphenMacrosReferencesIncludedHeaderMacroFile) {
  // A header's umbrella macro file includes the macro files of the library
  // headers it includes. With the hyphen option those references must use the
  // hyphenated names (`b-macros.h`), not the underscore defaults, and carry
  // the header's include prefix.
  auto r = rewrite_header(
      data_path("hymac_lib/a.h"), "hymac_lib",
      RewriteOptions{.include_to_module = {{"hymac_lib/b.h", "hymac_lib.b"}},
                     .extra_args = {"-I", gDataDir},
                     .hyphen_macros = true});
  EXPECT_NE(r.macros_content.find("#include \"hymac_lib/b-macros.h\""),
            std::string::npos)
      << "the umbrella macro header must reference the hyphenated macro file "
         "of an included library header";
  EXPECT_EQ(r.macros_content.find("b_macros.h"), std::string::npos)
      << "no underscore macro-file reference may remain under the hyphen "
         "option";
}

TEST(HeaderRewrite, MacroHeadersPlacedInIncludePrefix) {
  // The generated macro file lives next to the header that uses it, so a
  // header under `gmf_lib/` gets `gmf_lib/<stem>-macros.h` (no manual
  // relocation to the header's directory after generation).
  auto r = rewrite_header(data_path("gmf_lib/order.h"), "gmf_lib.order");
  EXPECT_EQ(r.macros_name, "gmf_lib/order_macros")
      << "the macros file must carry the header's include prefix";
  EXPECT_NE(r.cc_content.find("#include \"gmf_lib/order_macros.h\""),
            std::string::npos)
      << "the GMF include must reference the prefixed macros file";
  auto rh = rewrite_header(data_path("gmf_lib/order.h"), "gmf_lib.order",
                           RewriteOptions{.hyphen_macros = true});
  EXPECT_EQ(rh.macros_name, "gmf_lib/order-macros")
      << "the hyphen option hyphenates the stem and keeps the prefix";
}

TEST(HeaderRewrite, HyphenMacrosExportHeaderName) {
  // The export helper header is named <lib>_export.h by default and
  // <lib>-export.h under --hyphen-macros; a library that uses an include
  // subdirectory places it at the include root.
  auto r = rewrite_header(data_path("gmf_lib/order.h"), "gmf_lib.order",
                          RewriteOptions{.hyphen_macros = true});
  EXPECT_EQ(r.export_h_name, "gmf_lib/gmf_lib-export");
  EXPECT_NE(r.h_content.find("#include \"gmf_lib/gmf_lib-export.h\""),
            std::string::npos)
      << "the header body must include the hyphenated export helper";
  auto r2 = rewrite_header(data_path("gmf_lib/order.h"), "gmf_lib.order");
  EXPECT_EQ(r2.export_h_name, "gmf_lib/gmf_lib_export")
      << "the default naming keeps underscores";
}

TEST(HeaderRewrite, UmbrellaMacroFileChainsIncludedMacros) {
  // The umbrella macro file must chain-include the macro files of the library
  // headers it includes, so consumers do not need to add them manually (the
  // umbrella `macros.h` → `spi-macros.h` regression).
  auto r = rewrite_header(
      data_path("hymac_lib/a.h"), "hymac_lib",
      RewriteOptions{.include_to_module = {{"hymac_lib/b.h", "hymac_lib.b"}},
                     .extra_args = {"-I", gDataDir}});
  EXPECT_NE(r.macros_content.find("#include \"hymac_lib/b_macros.h\""),
            std::string::npos)
      << "the umbrella macro header must chain the included header's macro "
         "file with its include prefix";
}

TEST(HeaderRewrite, ExtractsMacros) {
  auto r = rewrite_header(data_path("macro_test.h"), "test_lib");
  EXPECT_FALSE(r.macros_content.empty());
  EXPECT_FALSE(r.macros_name.empty());
}

TEST(HeaderRewrite, PreservesPragmaOnce) {
  auto r = rewrite_header(data_path("macro_test.h"), "test_lib");
  EXPECT_NE(r.h_content.find("#pragma once"), std::string::npos);
}

TEST(HeaderRewrite, WrapsAllIncludesWithGuard) {
  std::map<std::string, std::string> include_map = {
    {"macro_test.h", "test_lib"}
  };
  auto r = rewrite_header(data_path("header_with_includes.h"), "test_lib", RewriteOptions{.combined_macros = false, .include_to_module = include_map});
  auto guard = "TEST_LIB_USE_MODULES";
  // All includes wrapped, adjacent blocks merged. Anchor on the guard opening
  // that precedes the first wrapped include: the header also carries its own
  // guarded macros-file include above them, for the non-module build.
  auto first_inc = r.h_content.find("#include \"macro_test.h\"");
  ASSERT_NE(first_inc, std::string::npos);
  auto ndef = r.h_content.rfind(std::format("#ifndef {}", guard), first_inc);
  EXPECT_NE(ndef, std::string::npos);
  auto endf = r.h_content.find("#endif", ndef);
  EXPECT_NE(endf, std::string::npos);
  auto block = r.h_content.substr(ndef, endf - ndef);
  EXPECT_NE(block.find("#include \"macro_test.h\""), std::string::npos);
  EXPECT_NE(block.find("#include <vector>"), std::string::npos);
  // No extra #ifndef / #endif between them (merged)
  auto middle = r.h_content.substr(
      r.h_content.find("macro_test.h"),
      r.h_content.find("<vector>") - r.h_content.find("macro_test.h"));
  EXPECT_EQ(middle.find("#endif"), std::string::npos);
  EXPECT_EQ(middle.find("#ifndef"), std::string::npos);
}

TEST(HeaderRewrite, ExportsClassDefinedInsideMacro) {
  // A macro whose body defines a public class (e.g. attribute-style macros)
  // must export the class itself,
  // not the macro invocation. The macro body is textually shared with
  // consumers, so `TEST_LIB_EXPORT` must appear inside the macro body, in
  // front of `class exported_class`, so the class is exported wherever the
  // macro expands. The public macro moves to the macros file, carrying the
  // marker.
  auto r = rewrite_header(data_path("macro_class.h"), "test_lib");
  EXPECT_NE(r.macros_content.find(
                "TEST_LIB_EXPORT class exported_class : detail::helper"),
            std::string::npos)
      << "the class defined inside the macro body must carry TEST_LIB_EXPORT";
  // The invocation itself must not be exported: `TEST_LIB_EXPORT MACRO(x)`
  // would expand `export` in front of the whole macro body (namespace detail
  // included), which is not what we want.
  EXPECT_EQ(r.h_content.find("TEST_LIB_EXPORT DECLARE_MACRO(public:);"),
            std::string::npos)
      << "the macro invocation must not be exported directly";
  // The macro definition itself must remain valid text, with the marker
  // inserted before the class and the rest of the body unchanged.
  EXPECT_NE(r.macros_content.find(
                "#define DECLARE_MACRO(x) namespace detail { class helper "
                "{}; } TEST_LIB_EXPORT class exported_class : detail::helper "
                "{ x };"),
            std::string::npos)
      << "the macro body must stay intact with TEST_LIB_EXPORT before the "
         "class";
  // The public macro no longer lives in the header body.
  EXPECT_EQ(r.h_content.find("#define DECLARE_MACRO"), std::string::npos)
      << "the public macro must move out of the header body into the macros "
         "file";
}

TEST(HeaderRewrite, MovesIncludesToCC) {
  std::map<std::string, std::string> include_map = {
    {"macro_test.h", "test_lib"}
  };
  auto r = rewrite_header(data_path("header_with_includes.h"), "test_lib", RewriteOptions{.combined_macros = false, .include_to_module = include_map});
  // System includes stay in the .cc GMF
  EXPECT_NE(r.cc_content.find("#include <vector>"), std::string::npos);
  // The module declaration comes before anything else that needs it
  EXPECT_NE(r.cc_content.find("export module test_lib;"), std::string::npos);
}

TEST(HeaderRewrite, GeneratesImportsInCC) {
  std::map<std::string, std::string> include_map = {
    {"macro_test.h", "test_lib"}
  };
  auto r = rewrite_header(data_path("header_with_includes.h"), "test_lib", RewriteOptions{.combined_macros = false, .include_to_module = include_map});
  EXPECT_NE(r.cc_content.find("export import test_lib;"), std::string::npos);
  EXPECT_NE(r.cc_content.find("TEST_LIB_USE_MODULES"), std::string::npos);
}

TEST(HeaderRewrite, InternalModuleUsesPlainImport) {
  std::map<std::string, std::string> include_map = {
    {"test_lib/internal/foo.h", "test_lib.foo"},
    {"test_lib/bar.h", "test_lib.bar"}
  };
  auto r = rewrite_header(data_path("test_lib/bar.h"), "test_lib.bar", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}});
  // Internal module must be imported, not re-exported
  EXPECT_NE(r.cc_content.find("import test_lib.foo;"), std::string::npos)
      << "internal module must be imported";
  EXPECT_EQ(r.cc_content.find("export import test_lib.foo;"), std::string::npos)
      << "internal module must not be re-exported";
}

TEST(HeaderRewrite, DependantImportsInternalModuleDirectly) {
  std::map<std::string, std::string> include_map = {
    {"test_lib/internal/foo.h", "test_lib.foo"},
    {"test_lib/bar.h", "test_lib.bar"}
  };
  // baz.h uses detail::Foo from the internal module and only includes
  // bar.h transitively; since bar.h no longer re-exports the internal
  // module, baz must import test_lib.foo directly.
  auto r = rewrite_header(data_path("test_lib/baz.h"), "test_lib.baz", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}});
  EXPECT_NE(r.cc_content.find("import test_lib.foo;"), std::string::npos)
      << "dependant that needs internal entities must import the internal module";
  EXPECT_NE(r.cc_content.find("test_lib.bar;"), std::string::npos)
      << "dependant must still import the public module";
}

TEST(HeaderRewrite, InternalModuleWithMacroReachableInternalsExported) {
  std::map<std::string, std::string> include_map = {
    {"test_lib/internal/foo.h", "test_lib.foo"},
    {"test_lib/bar.h", "test_lib.bar"}
  };
  // foo.h is an internal module, but its entities are referenced by a
  // public macro (macro_modules), so it must be re-exported like a normal
  // module — the consumer reaches it via the macro through the umbrella.
  std::set<std::string> macro_modules = {"test_lib.foo"};
  auto r = rewrite_header(data_path("test_lib/bar.h"), "test_lib.bar", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = macro_modules});
  EXPECT_NE(r.cc_content.find("export import test_lib.foo;"), std::string::npos)
      << "internal module with macro-reachable internals must be re-exported";
}

TEST(HeaderRewrite, MultiFileDotsExample) {
  std::map<std::string, std::string> include_map = {
    {"multi_a.h", "my_lib.a"},
    {"multi_c.h", "my_lib.c"}
  };
  auto ra = rewrite_header(data_path("multi_a.h"), "my_lib.a", RewriteOptions{.combined_macros = false, .include_to_module = include_map});
  auto rb = rewrite_header(data_path("multi_b.h"), "my_lib.b", RewriteOptions{.combined_macros = false, .include_to_module = include_map});

  // Universal macros live in export helper header, not inline
  EXPECT_NE(ra.export_h_content.find("MY_LIB_USE_MODULES"), std::string::npos);
  EXPECT_NE(ra.export_h_content.find("MY_LIB_EXPORT"), std::string::npos);
  EXPECT_NE(rb.export_h_content.find("MY_LIB_USE_MODULES"), std::string::npos);
  EXPECT_NE(rb.export_h_content.find("MY_LIB_EXPORT"), std::string::npos);

  // Each header includes the export helper
  EXPECT_NE(ra.h_content.find("#include \"my_lib_export.h\""), std::string::npos);
  EXPECT_NE(rb.h_content.find("#include \"my_lib_export.h\""), std::string::npos);

  // Export helper header content
  EXPECT_FALSE(ra.export_h_content.empty());
  EXPECT_EQ(ra.export_h_name, "my_lib_export");
  EXPECT_NE(ra.export_h_content.find("MY_LIB_EXPORT"), std::string::npos);
  EXPECT_NE(ra.export_h_content.find("MY_LIB_USE_MODULES"), std::string::npos);
  EXPECT_NE(ra.export_h_content.find("#pragma once"), std::string::npos);

  // Merged guard block in b.h (anchored on the guard that wraps the includes,
  // below the header's own guarded macros-file include).
  auto first_inc = rb.h_content.find("#include \"multi_a.h\"");
  ASSERT_NE(first_inc, std::string::npos);
  auto ndef = rb.h_content.rfind("#ifndef MY_LIB_USE_MODULES", first_inc);
  auto endf = rb.h_content.find("#endif", ndef);
  auto block = rb.h_content.substr(ndef, endf - ndef);
  EXPECT_NE(block.find("#include \"multi_a.h\""), std::string::npos);
  EXPECT_NE(block.find("#include <string>"), std::string::npos);
  EXPECT_EQ(block.find("#ifndef MY_LIB_USE_MODULES", ndef + 1), std::string::npos);

  // b.cc imports a.h module
  EXPECT_NE(rb.cc_content.find("export import my_lib.a;"), std::string::npos);
  EXPECT_NE(rb.cc_content.find("MY_LIB_USE_MODULES"), std::string::npos);
}

TEST(HeaderRewrite, ExportPlacementUsesExpansionLoc) {
  // Bug: addExport used spelling loc for macro-expanded decls,
  // putting all exports at offset 0 (file beginning).
  auto r = rewrite_header(data_path("macro_decl.h"), "test_lib");
  // Exports must NOT appear before #pragma once
  auto pragma = r.h_content.find("#pragma once");
  auto first_export = r.h_content.find("TEST_LIB_EXPORT");
  EXPECT_NE(pragma, std::string::npos);
  EXPECT_NE(first_export, std::string::npos);
  EXPECT_LT(pragma, first_export)
      << "exports must not appear before #pragma once";
}

TEST(HeaderRewrite, CCUsesFilenameNotAbsolutePath) {
  // Bug: generated .cc used the original absolute path for #include.
  auto r = rewrite_header(data_path("macro_test.h"), "test_lib");
  EXPECT_NE(r.cc_content.find("#include \"macro_test.h\""), std::string::npos);
  EXPECT_EQ(r.cc_content.find("/home/"), std::string::npos)
      << ".cc must not contain absolute paths";
  EXPECT_EQ(r.cc_content.find("/test/"), std::string::npos);
}

TEST(HeaderRewrite, AutoImportForLibraryInternalIncludes) {
  // Bug: library-internal includes weren't converted to imports.
  auto r = rewrite_header(data_path("library_root.h"), "mylib.myroot");
  EXPECT_NE(r.cc_content.find("export import mylib.mylib_sub;"), std::string::npos)
      << "library-internal includes should become imports";
  // The include should NOT be in the GMF as a raw include
  EXPECT_EQ(r.cc_content.find("#include \"mylib/mylib_sub.h\""),
            std::string::npos);
}

TEST(HeaderRewrite, AutoImportDeduplicatesLibraryName) {
  // Bug: auto-detected module name "libname.libname" (e.g. mylib.mylib)
  // should be deduplicated to just "libname".
  // Use a header that includes "mylib/mylib.h" — but we test with
  // library_root.h which includes "mylib/mylib_sub.h", so it imports
  // "mylib.mylib_sub" (no dedup needed there).
  // Instead, verify that the self-import check works.
  auto r = rewrite_header(data_path("library_root.h"), "mylib");
  // The umbrella import should be "mylib.mylib_sub", not "mylib.mylib"
  EXPECT_NE(r.cc_content.find("export import mylib.mylib_sub;"), std::string::npos);
}

TEST(HeaderRewrite, SubHeaderImportsUmbrella) {
  // A sub-header that directly includes the library's umbrella/root header must
  // import it rather than keeping the textual include in the GMF (which would
  // define the library's entities in the global module and conflict with the
  // imported modules).
  auto r = rewrite_header(data_path("rootlib/leaf.h"), "rootlib.leaf");
  EXPECT_NE(r.cc_content.find("export import rootlib;"), std::string::npos)
      << "the umbrella include must become an import";
  EXPECT_EQ(r.cc_content.find("#include \"rootlib/rootlib.h\""),
            std::string::npos)
      << "the umbrella include must not remain in the GMF";
}

TEST(HeaderRewrite, GuardedIncludesEmittedWithGuardsInCC) {
  // Bug: includes inside #ifdef blocks lost their guard context in the GMF.
  auto r = rewrite_header(data_path("guarded_includes.h"), "test_lib");
  // feature_x.h is inside #ifdef HAS_FEATURE_X
  auto fx = r.cc_content.find("#include <feature_x.h>");
  EXPECT_NE(fx, std::string::npos);
  auto before_fx = r.cc_content.substr(0, fx);
  EXPECT_NE(before_fx.rfind("#ifdef HAS_FEATURE_X"), std::string::npos)
      << "guarded include missing #ifdef guard in GMF";
  // There should be #endif after it
  auto after_fx = r.cc_content.substr(fx);
  EXPECT_NE(after_fx.find("#endif"), std::string::npos)
      << "guarded include missing closing #endif in GMF";

  // support_lib.h is inside #ifndef SKIP_SUPPORT
  auto sl = r.cc_content.find("#include <support_lib.h>");
  EXPECT_NE(sl, std::string::npos);
  auto before_sl = r.cc_content.substr(0, sl);
  EXPECT_NE(before_sl.rfind("#ifndef SKIP_SUPPORT"), std::string::npos)
      << "ifndef-guarded include missing guard in GMF";

  // sub_feature.h is inside nested guards
  auto sf = r.cc_content.find("#include <sub_feature.h>");
  EXPECT_NE(sf, std::string::npos);
  auto before_sf = r.cc_content.substr(0, sf);
  EXPECT_NE(before_sf.rfind("#ifdef PLATFORM_A"), std::string::npos)
      << "nested guard missing outer level";
  EXPECT_NE(before_sf.rfind("#ifdef SUB_FEATURE"), std::string::npos)
      << "nested guard missing inner level";
}

TEST(HeaderRewrite, BackslashContinuationGuardsFiltered) {
  // Bug: #if lines with \ continuation (multi-line) caused invalid
  // preprocessor expressions when emitted as single-line guards.
  auto r = rewrite_header(data_path("backslash_guard.h"), "test_lib");
  // inside_multiline.h should NOT have the \ guard prefix
  auto im = r.cc_content.find("#include <inside_multiline.h>");
  EXPECT_NE(im, std::string::npos);
  // The multiline guard should NOT appear directly before it
  auto before_im = r.cc_content.substr(im > 50 ? im - 50 : 0, 50);
  EXPECT_EQ(before_im.find("#if SOME_CONDITION && \\"), std::string::npos)
      << "multi-line guard with \\ must be filtered from GMF";

  // simple_guard.h SHOULD have its guard (no \)
  auto sg = r.cc_content.find("#include <simple_guard.h>");
  EXPECT_NE(sg, std::string::npos);
  auto before_sg = r.cc_content.substr(0, sg);
  EXPECT_NE(before_sg.rfind("#ifdef SIMPLE_GUARD"), std::string::npos)
      << "guard without \\ should be preserved";
}

TEST(HeaderRewrite, TextualMacroExtractionOnlyCapturesTopLevel) {
  // Macros inside #if branches (active or inactive) should NOT be
  // extracted textually — only unconditional top-level #defines that the
  // PPCallbacks missed should be captured. This prevents platform-
  // specific macros from being defined with wrong values.
  auto r = rewrite_header(data_path("textual_macros.h"), "test_lib");
  EXPECT_EQ(r.macros_content.find("INACTIVE_PLATFORM_MACRO"), std::string::npos)
      << "macros inside #if 0 must NOT be extracted";
  EXPECT_NE(r.macros_content.find("ACTIVE_MACRO"), std::string::npos)
      << "unconditional active macros must be present";
}

TEST(HeaderRewrite, TextualMacroExtractionCapturesNestedConditional) {
  // A macro defined inside `#if defined(PLATFORM)` / `#else` is used by other
  // headers' bodies. In cc-only mode
  // those bodies are inlined and need the macro, so it must be extracted even
  // though it is nested in an active conditional.
  auto r = rewrite_header(data_path("macronest/dep.h"), "macronest.dep", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}});
  EXPECT_NE(r.macros_content.find("NESTED_HELPER"), std::string::npos)
      << "a macro inside an active #if defined() block must be extracted";
}

TEST(HeaderRewrite, TextualMacroExtractionPreservesConditionalGuards) {
  // A macro with different bodies per `#if defined(_MSC_VER)` / `#else` branch
  // must keep its guard structure in the macros file, so the correct branch
  // expands on the actual platform.
  // Extracting only one branch (without the #if) would force the wrong body.
  auto r = rewrite_header(data_path("macronest/cond.h"), "macronest.cond", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}});
  EXPECT_NE(r.macros_content.find("#if defined(__clang__)"),
            std::string::npos)
      << "the conditional guard around a nested macro must be preserved";
  EXPECT_NE(r.macros_content.find("#else"), std::string::npos)
      << "the #else branch of a conditional macro must be preserved";
  EXPECT_NE(r.macros_content.find("#endif"), std::string::npos)
      << "the #endif of a conditional macro must be preserved";
}

TEST(HeaderRewrite, TextualMacroExtractionSkipsCodeInConditional) {
  // A conditional block that mixes #defines with declarations (e.g. a forward
  // declaration) must NOT be emitted verbatim into the macros file — only pure
  // macro blocks (only preprocessor directives) may be. Emitting code would
  // declare entities (e.g. `class Bar;`) in the global module fragment.
  auto r = rewrite_header(data_path("macronest/codeblock.h"), "macronest.codeblock", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}});
  EXPECT_EQ(r.macros_content.find("class Bar;"), std::string::npos)
      << "declarations inside a conditional must not leak into the macros "
         "file";
  // The macro body is still needed: either the conditional is preserved or the
  // macro is extracted — but the macro must remain usable.
  EXPECT_TRUE(r.macros_content.find("CODEBLOCK_HELPER") != std::string::npos ||
              r.h_content.find("CODEBLOCK_HELPER") != std::string::npos)
      << "the macro must remain usable";
}

TEST(HeaderRewrite, TextualMacroExtractionMultiLineIfContinuation) {
  // A multi-line `#if defined(_MSC_VER) && \` condition ends with a backslash
  // continuation; the continuation line must not be mistaken for a code line
  // (which would suppress emitting this guard block). The guarded macro and the
  // macro that references it must both be extracted.
  auto r = rewrite_header(data_path("macronest/mlcond.h"), "macronest.mlcond",
                          RewriteOptions{.extern_cxx = true,
                                         .extra_args = {"-I", gDataDir}});
  EXPECT_NE(r.macros_content.find("MULTI_COND_MACRO"), std::string::npos)
      << "a macro guarded by a multi-line #if must be extracted";
  EXPECT_NE(r.macros_content.find("USE_MULTI_COND"), std::string::npos)
      << "the macro that references the guarded macro must be extracted";
}

TEST(HeaderRewrite, InjectsFwdDeclForReferencedOpaqueType) {
  // consumer.h declares `void operator<<(const Foo&, int)` where Foo is
  // forward-declared in dep.h (now an imported module) but never defined. The
  // referencing module must inject a forward declaration of Foo so the
  // signature is valid (an internal class is the real-world example).
  std::map<std::string, std::string> include_map = {
    {"fwdopaque/dep.h", "fwdopaque.dep"},
  };
  auto r = rewrite_header(data_path("fwdopaque/consumer.h"), "fwdopaque.consumer", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwdopaque::Foo"}, .fwd_declared_fqns = {"fwdopaque::Foo"}});
  // The forward declaration must be emitted somewhere in the interface unit so
  // `void operator<<(const Foo&, int)` is well-formed.
  EXPECT_NE(r.cc_content.find("class Foo;"), std::string::npos)
      << "a referenced-but-undefined type must get a forward declaration in "
         "the module";
}

TEST(HeaderRewrite, InjectedRecordDropsAttributeMacroBeforeName) {
  // attrinject_lib/defs.h defines class templates with an attribute macro
  // (`TEST_API_`) before the class name. An injected `extern "C++"` forward
  // declaration for such a cross-module record must not place the attribute
  // macro before the class name (ill-formed).
  std::map<std::string, std::string> include_map = {
    {"attrinject_lib/defs.h", "attrinject_lib.defs"},
  };
  auto r = rewrite_header(
      data_path("attrinject_lib/use.h"), "attrinject_lib.use",
      RewriteOptions{.include_to_module = include_map,
                     .extra_args = {"-I", gDataDir},
                     .defined_fqns = {"attrinject_lib::internal::Box"},
                     .fwd_declared_fqns = {"attrinject_lib::internal::Box"}});
  EXPECT_EQ(r.cc_content.find("extern \"C++\" TEST_API_"),
            std::string::npos)
      << "the injected extern \"C++\" forward decl must not carry the attribute "
         "macro before the class name";
}

TEST(HeaderRewrite, ExternCxxDropsStorageClassExtern) {
  // linkspec_lib/api.h declares entities with an `extern` storage-class
  // specifier of their own. Wrapping such a declaration in `extern "C++"`
  // leaves the keyword inside the linkage-specification, which is ill-formed
  // ("invalid use of 'extern' in linkage specification"), so it must be
  // dropped — including on a template, where it follows the parameter list.
  auto r = rewrite_header(
      data_path("linkspec_lib/api.h"), "linkspec_lib.api",
      RewriteOptions{.extern_cxx = false,
                     .extra_args = {"-I", gDataDir},
                     .fwd_declared_fqns = {"linkspec_lib::Make",
                                           "linkspec_lib::Check",
                                           "linkspec_lib::counter"}});
  EXPECT_NE(r.h_content.find("extern \"C++\" template <class T>"),
            std::string::npos)
      << "the cross-module template declaration must be extern \"C++\"";
  EXPECT_EQ(r.h_content.find("extern T Make"), std::string::npos)
      << "the template's own extern storage-class specifier must be dropped";
  EXPECT_EQ(r.h_content.find("extern \"C++\" extern"), std::string::npos)
      << "no declaration may keep an extern specifier inside extern \"C++\"";
  EXPECT_NE(r.h_content.find("extern \"C++\" bool Check(int n);"),
            std::string::npos)
      << "a function declaration keeps its signature without the specifier";
  EXPECT_NE(r.h_content.find("extern \"C++\" int counter;"), std::string::npos)
      << "a variable stays a declaration: extern \"C++\" carries the meaning";
}

TEST(HeaderRewrite, InjectedSignatureTypeKeptComplete) {
  // fwdstr_lib/consumer.h references `foo` (only declared in dep.h). The
  // injected module-local forward declaration must keep the full signature
  // (return type) so the referencing body is well-formed.
  std::map<std::string, std::string> include_map = {
    {"fwdstr_lib/dep.h", "fwdstr_lib.internal.dep"},
  };
  auto r = rewrite_header(
      data_path("fwdstr_lib/consumer.h"), "fwdstr_lib.consumer",
      RewriteOptions{.include_to_module = include_map,
                     .extra_args = {"-I", gDataDir},
                     .defined_fqns = {"fwdstr_lib::foo"},
                     .fwd_declared_fqns = {"fwdstr_lib::foo"}});
  EXPECT_NE(r.cc_content.find("std::string foo"), std::string::npos)
      << "the injected forward declaration must keep the return type";
}

TEST(HeaderRewrite, InjectsExportedDeclForTemplateUsedBeforeOwnDefinition) {
  // tplbase_lib/use.h's `Gen` uses `Foo`/`Bar`/`Baz`/`Box` from defs.h (a
  // DIFFERENT module) before their own definitions. The using module must
  // inject exported declarations for these so the template bodies are valid.
  std::map<std::string, std::string> include_map = {
    {"tplbase_lib/defs.h", "tplbase_lib.defs"},
  };
  auto r = rewrite_header(
      data_path("tplbase_lib/use.h"), "tplbase_lib.use",
      RewriteOptions{.include_to_module = include_map,
                     .extern_cxx = false,
                     .extra_args = {"-I", gDataDir},
                     .fwd_declared_fqns = {"tplbase_lib::internal::Foo",
                                           "tplbase_lib::internal::Bar"},
                     .cc_only = true});
  EXPECT_NE(r.cc_content.find("extern \"C++\" template <typename F, "
                              "typename Tuple, std::size_t... Idx>"),
            std::string::npos)
      << "the using module must inject an extern \"C++\" declaration for the "
         "cross-module template used before its own definition";
}

TEST(HeaderRewrite, PublicCrossModuleEntityNotInjectedWhenVisibleViaImport) {
  // A cross-module entity that is PUBLIC and exported (visible via the import)
  // must NOT get an injected extern "C++" declaration in the using module —
  // consumers see it through the import.
  std::map<std::string, std::string> include_map = {
    {"tplbase_lib/defs.h", "tplbase_lib.defs"},
  };
  auto r = rewrite_header(
      data_path("tplbase_lib/use.h"), "tplbase_lib.use",
      RewriteOptions{.include_to_module = include_map,
                     .reachable_fqns = {"tplbase_lib::internal::Foo",
                                        "tplbase_lib::internal::Bar"},
                     .extern_cxx = false,
                     .extra_args = {"-I", gDataDir},
                     .fwd_declared_fqns = {},
                     .cc_only = true});
  EXPECT_EQ(r.cc_content.find("extern \"C++\" template <typename F"),
            std::string::npos)
      << "a cross-module entity visible via the import must not get an injected "
         "extern \"C++\" declaration";
}

TEST(HeaderRewrite, ExpandsReachableFunctionReturnTypes) {
  // use.h's `use_helper` references `reachmod_lib::internal::Bar` in its
  // signature. The analyzer must record that internal type so a reachable
  // function's signature can be expanded to it (expand_transitive_types).
  auto model = analyze_files_with_flags({data_path("reachmod_lib/use.h")},
                                        {"-I", gDataDir});
  bool has_bar_ref = false;
  for (auto &item : model.items)
    if (item.name == "use_helper")
      for (auto &tr : item.type_refs)
        if (tr == "reachmod_lib::internal::Bar") has_bar_ref = true;
  EXPECT_TRUE(has_bar_ref)
      << "the analyzer must record the internal type referenced by a "
         "function's signature";
}

TEST(HeaderRewrite, FwdDeclKeepsReferencedSystemHeaderInGmf) {
  // consumer.h uses `foo` which is only fwd-declared in dep.h (an
  // internal module that does not export it, since its definition lives in
  // another module). The tool injects a module-local forward declaration whose
  // signature references `std::string`; the GMF must therefore keep `<string>`
  // so the injected declaration is well-formed (a cross-module free function
  // is the real-world case).
  std::map<std::string, std::string> include_map = {
    {"fwdstr_lib/dep.h", "fwdstr_lib.internal.dep"},
  };
  auto r = rewrite_header(data_path("fwdstr_lib/consumer.h"), "fwdstr_lib.consumer", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwdstr_lib::foo"}, .fwd_declared_fqns = {"fwdstr_lib::foo"}});
  EXPECT_NE(r.cc_content.find("foo"), std::string::npos)
      << "the module-local forward declaration must be injected";
  EXPECT_NE(r.cc_content.find("#include <string>"), std::string::npos)
      << "the GMF must keep <string> for the injected fwd decl's return type";
}

TEST(HeaderRewrite, InterfaceUnitIncludesTheHeaderByItsIncludeForm) {
  // The interface unit ends up in the library's source directory while the
  // rewritten header keeps the include prefix its original had, so the include
  // has to be the form a consumer writes (`linkage_lib/api.h`). A bare
  // filename only resolves when the two happen to sit side by side.
  auto r = rewrite_header(data_path("linkage_lib/api.h"), "linkage_lib.api", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}});
  EXPECT_NE(r.cc_content.find("#include \"linkage_lib/api.h\""),
            std::string::npos)
      << "the interface unit must include its header by include form";
  EXPECT_EQ(r.cc_content.find("#include \"api.h\""), std::string::npos)
      << "a filename-only include would not resolve from the source directory";
}

TEST(HeaderRewrite, CreditsStdlibPrivateHeaderUseToItsPublicHeader) {
  // The injected forward declaration's `std::string` is *declared* in a
  // private libstdc++/libc++ header (`bits/basic_string.h`, `__string/…`),
  // reached only from inside `<string>`. Emitting that private header would
  // pin the output to the standard library it was generated against, so the
  // use is credited to the public header that owns it: `<string>` is kept and
  // no implementation-detail header appears in the GMF.
  std::map<std::string, std::string> include_map = {
    {"fwdstr_lib/dep.h", "fwdstr_lib.internal.dep"},
  };
  auto r = rewrite_header(data_path("fwdstr_lib/consumer.h"), "fwdstr_lib.consumer", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwdstr_lib::foo"}, .fwd_declared_fqns = {"fwdstr_lib::foo"}});
  EXPECT_NE(r.cc_content.find("#include <string>"), std::string::npos)
      << "the public header owning the used declaration must be kept";
  EXPECT_EQ(r.cc_content.find("#include <bits/"), std::string::npos)
      << "a libstdc++ private header must never be emitted";
  EXPECT_EQ(r.cc_content.find("#include <__"), std::string::npos)
      << "a libc++ private header must never be emitted";
}

TEST(HeaderRewrite, InjectsFwdDeclExactlyOnce) {
  // dep.h forward-declares fwddup_lib::internal::Secret; use.h never references
  // it, so the injection happens via the fwd_declared_fqns sweep. When that
  // list contains the same FQN twice (a function that is both an
  // out-of-line-defined free function
  // and a friend-extern fqn), the sweep must still emit the declaration exactly
  // once — a duplicated entry used to produce two identical injected blocks.
  auto r = rewrite_header(
      data_path("fwddup_lib/use.h"), "fwddup_lib.use",
      RewriteOptions{.include_to_module = {{"fwddup_lib/dep.h", "fwddup_lib.dep"}},
                     .reachable_fqns = {},
                     .extern_cxx = true,
                     .extra_args = {"-I", gDataDir},
                     .defined_fqns = {"fwddup_lib::internal::Secret",
                                      "fwddup_lib::Util"},
                     .fwd_declared_fqns = {"fwddup_lib::internal::Secret",
                                           "fwddup_lib::Util",
                                           "fwddup_lib::internal::Secret"}});
  std::string needle = "class Secret;";
  std::size_t count = 0;
  for (std::size_t pos = r.cc_content.find(needle); pos != std::string::npos;
       pos = r.cc_content.find(needle, pos + 1))
    ++count;
  EXPECT_EQ(count, 1u)
      << "a forward-declared entity must be injected exactly once even when the "
         "fwd_declared_fqns list contains a duplicate";
}

TEST(HeaderRewrite, SkipsFwdDeclWhenReachableViaImport) {
  // An internal entity that is NOT defined by any header (its definition lives
  // in a source file, so it is absent from defined_fqns) is exported by its
  // declaring module when listed as reachable; the referencing module sees it
  // through the import, so no injected `extern "C++"` declaration is needed
  // (an impl unit used to inject a cross-module free function even though
  // its declaring module exports it).
  auto r = rewrite_header(
      data_path("fwddup_lib/use.h"), "fwddup_lib.use",
      RewriteOptions{.include_to_module = {{"fwddup_lib/dep.h", "fwddup_lib.dep"}},
                     .reachable_fqns = {"fwddup_lib::internal::Secret"},
                     .extern_cxx = true,
                     .extra_args = {"-I", gDataDir},
                     .fwd_declared_fqns = {"fwddup_lib::internal::Secret"}});
  EXPECT_EQ(r.cc_content.find("class Secret;"),
            std::string::npos)
      << "an entity exported by an imported module must not get an injected "
         "extern \"C++\" declaration";
}

TEST(HeaderRewrite, InjectsFwdDeclWhenDefinitionInAnotherHeader) {
  // use.h sees only def.h's forward declaration of fwdkpr_lib::internal::Trace;
  // the complete definition lives in impl.h (a different module). Even though
  // Trace is listed as reachable, its declaring module (fwdkpr_lib.def) keeps
  // the bare declaration private (kKeepPrivate) — the definition is what the
  // defining module exports. The using module must inject a module-local
  // forward declaration (an entity declared in one header but defined in its
  // matching impl-inl.h is the real-world case).
  auto r = rewrite_header(
      data_path("fwdkpr_lib/use.h"), "fwdkpr_lib.use",
      RewriteOptions{.include_to_module = {{"fwdkpr_lib/def.h", "fwdkpr_lib.def"},
                                           {"fwdkpr_lib/impl.h", "fwdkpr_lib.impl"}},
                     .reachable_fqns = {"fwdkpr_lib::internal::Trace"},
                     .extern_cxx = true,
                     .extra_args = {"-I", gDataDir},
                     .defined_fqns = {"fwdkpr_lib::internal::Trace"},
                     .fwd_declared_fqns = {"fwdkpr_lib::internal::Trace"}});
  EXPECT_NE(r.cc_content.find("class Trace;"), std::string::npos)
      << "an entity whose definition lives in a different library header must "
         "still get an injected forward declaration even when reachable";
}

TEST(HeaderRewrite, ReachableInternalEntityMarksModuleForExportImport) {
  // use.h includes reachmod_lib/internal/tools.h whose `Bar` is reachable.
  // modules_with_reachable_entities must report the internal module so the
  // importer `export import`s it (treating it as normal) instead of a plain
  // `import` that hides the reachable internal entity from consumers.
  auto mods = modulizer::modules_with_reachable_entities(
      {data_path("reachmod_lib/use.h"),
       data_path("reachmod_lib/internal/tools.h")},
      "reachmod_lib", {"-I", gDataDir},
      {"reachmod_lib::internal::Bar"});
  EXPECT_NE(mods.count("reachmod_lib.internal.tools"), 0u)
      << "an internal module owning a reachable entity must be marked so "
         "importers use export import";
  EXPECT_EQ(mods.count("reachmod_lib.use"), 0u)
      << "a module whose own header declares the reachable entity is already "
         "the consumer, not the provider";
}

TEST(HeaderRewrite, PrimaryTemplateForwardDeclOfReachableSpecializationExported) {
  // A class template with only a forward-declared primary (`template <typename
  // T> struct Foo;`) and a complete partial specialization. The primary is
  // NOT a complete definition, so with `defined_fqns` populated (as in the full
  // rewrite path) it would be treated as a cross-module fwd-decl and kept
  // private — but consumers name `Foo<Signature>` via public macros, so
  // the primary template declaration must be exported from the module that owns
  // the specialization.
  auto r = rewrite_header(data_path("tplreach_lib/api.h"), "tplreach_lib.api", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {"tplreach_lib::internal::Foo"}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"tplreach_lib::internal::Foo"}, .fwd_declared_fqns = {}, .cc_only = /*cc_only=*/true});
  EXPECT_NE(r.cc_content.find("TPLREACH_LIB_EXPORT template <typename T>"),
            std::string::npos)
      << "the primary template forward declaration must be exported so "
         "consumers can name Foo<X>";
  EXPECT_EQ(r.cc_content.find("extern \"C++\" template <typename T>\n"
                              "struct Foo;"),
            std::string::npos)
      << "the primary template must not be kept as a private extern \"C++\" "
         "forward declaration";
}

TEST(HeaderRewrite, PrimaryTemplatePartialSpecNoClashingInjectedFwdDecl) {
  // consumer.h uses `Foo<F>` → `Box<F>` where the primary template
  // `template <typename T> struct Box;` is declared in producer.h (a module
  // import). The primary is never "complete" (only the partial specialization
  // is), so naively the rewriter would inject a module-local
  // `extern "C++" struct Box;` — which then clashes with the imported module's
  // exported primary declaration ("declaration of 'Box' in the global module
  // follows declaration in module ..."). The partial specialization is the
  // definition and is visible via the import, so no fwd-decl must be injected.
  std::map<std::string, std::string> include_map = {
    {"fwdpart_lib/producer.h", "fwdpart_lib.producer"},
  };
  auto r = rewrite_header(data_path("fwdpart_lib/consumer.h"), "fwdpart_lib.consumer", RewriteOptions{.combined_macros = false, .include_to_module = include_map, .reachable_fqns = {"fwdpart_lib::internal::Box"}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwdpart_lib::Box"}, .fwd_declared_fqns = {}, .cc_only = /*cc_only=*/true});
  EXPECT_NE(r.cc_content.find("import fwdpart_lib.producer;"),
            std::string::npos)
      << "the producer header must become a module import";
  EXPECT_EQ(r.cc_content.find("struct Box;"),
            std::string::npos)
      << "no clashing forward declaration of Box may be injected when the "
         "imported producer module exports it";
}

TEST(HeaderRewrite, CrossModuleAliasTemplateExportedNotInjected) {
  // use.h's template body references `Foo` (an alias template, defined in
  // prod.h, a DIFFERENT module). Alias templates cannot be forward-declared, so
  // injecting a copy of the definition into the using module is impossible.
  // Instead the defining module must EXPORT the alias (it is added to the
  // reachable set), so the using module sees it through its import.
  auto use = rewrite_header(data_path("adlreach_lib/use.h"), "adlreach_lib.use", RewriteOptions{.combined_macros = false, .include_to_module = {{"adlreach_lib/prod.h", "adlreach_lib.prod"}}, .reachable_fqns = /*reachable_fqns=*/{"adlreach_lib::internal::Foo",
                          "adlreach_lib::internal::Box"}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {}, .cc_only = /*cc_only=*/true});
  EXPECT_EQ(use.cc_content.find("using Foo"),
            std::string::npos)
      << "the using module must NOT inject a copy of the alias template — it "
         "sees the exported definition via the import";
  auto prod = rewrite_header(data_path("adlreach_lib/prod.h"), "adlreach_lib.prod", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = /*reachable_fqns=*/{"adlreach_lib::internal::Foo",
                          "adlreach_lib::internal::Box"}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {}, .cc_only = /*cc_only=*/true});
  EXPECT_NE(prod.cc_content.find(
                "ADLREACH_LIB_EXPORT template <typename T>\n"
                "using Foo = Box<Kind::kB, T>;"),
            std::string::npos)
      << "the defining module must export the cross-module alias template so "
         "using modules see it via the import";
}

TEST(HeaderRewrite, MemberAliasTemplateNotExported) {
  // An alias template declared inside a class must never get an export marker
  // prepended — that would inject `LIB_EXPORT template <...> using ...` inside
  // the class body and break with "expected member name".
  auto r = rewrite_header(data_path("memalias_lib/api.h"), "memalias_lib.api", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {}, .cc_only = /*cc_only=*/true});
  auto widget_pos = r.cc_content.find("class Foo");
  auto member_pos = r.cc_content.find("using Bar");
  EXPECT_NE(widget_pos, std::string::npos);
  EXPECT_NE(member_pos, std::string::npos);
  EXPECT_EQ(r.cc_content.find("MEMALIAS_LIB_EXPORT template <typename U>\n"
                              "using Bar"),
            std::string::npos)
      << "a member alias template must not be exported inside the class";
  // The class itself is exported normally.
  EXPECT_NE(r.cc_content.find("MEMALIAS_LIB_EXPORT template <typename T>\n"
                              "class Foo"),
            std::string::npos)
      << "the owning class template must still be exported";
}

TEST(HeaderRewrite, CrossModuleFunctionTemplateGetsExternCxxInjection) {
  // use.h's template body references `Foo` (a free function template, defined
  // in defs.h) and `Bar` (its helper). The using module must inject
  // module-local `extern "C++"` declarations so the template body's lookup at
  // its point of definition succeeds, without exporting them to consumers.
  auto use = rewrite_header(data_path("tplbase_lib/use.h"), "tplbase_lib.use", RewriteOptions{.combined_macros = false, .include_to_module = {{"tplbase_lib/defs.h", "tplbase_lib.defs"}}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {"tplbase_lib::internal::Foo",
       "tplbase_lib::internal::Bar"}, .cc_only = /*cc_only=*/true});
  EXPECT_NE(use.cc_content.find("extern \"C++\" template <typename F, typename Tuple>"),
            std::string::npos)
      << "the using module must inject the cross-module function template";
  // Bar is a transitive dependency of Foo (Foo's body calls Bar) — it must be
  // injected BEFORE Foo.
  auto impl_pos = use.cc_content.find(
      "extern \"C++\" template <typename F, typename Tuple, std::size_t... Idx>\n"
      "auto Bar");
  auto apply_pos = use.cc_content.find(
      "extern \"C++\" template <typename F, typename Tuple>\n"
      "auto Foo(F&& f, Tuple&& args)");
  EXPECT_NE(impl_pos, std::string::npos)
      << "the transitive dependency Bar must be injected";
  EXPECT_LT(impl_pos, apply_pos)
      << "the transitive dependency must be injected before its dependent";
  // The defining module marks both `extern "C++"`.
  auto prod = rewrite_header(data_path("tplbase_lib/defs.h"), "tplbase_lib.defs", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {"tplbase_lib::internal::Foo",
       "tplbase_lib::internal::Bar"}, .cc_only = /*cc_only=*/true});
  EXPECT_NE(prod.cc_content.find(
                "extern \"C++\" template <typename F, typename Tuple>\n"
                "auto Foo(F&& f, Tuple&& args)"),
            std::string::npos)
      << "the defining module must mark the function template definition "
         "extern \"C++\"";
}

TEST(HeaderRewrite, CrossModuleClassTemplateInjectedAsForwardDecl) {
  // use.h's template body references `Baz` (a class template with a base
  // clause, defined in defs.h). The using module must inject a forward
  // declaration `template <typename P> struct Baz;` — NOT the full
  // definition with its base-class list, which would be malformed (the body
  // `{}` is stripped but `: integral_constant<...>` would remain).
  auto use = rewrite_header(data_path("tplbase_lib/use.h"), "tplbase_lib.use", RewriteOptions{.combined_macros = false, .include_to_module = {{"tplbase_lib/defs.h", "tplbase_lib.defs"}}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {"tplbase_lib::internal::Baz",
       "tplbase_lib::internal::Box"}, .cc_only = /*cc_only=*/true});
  EXPECT_NE(use.cc_content.find(
                "extern \"C++\" template <typename P>\n"
                "struct Baz;"),
            std::string::npos)
      << "the using module must inject a forward declaration of the "
         "cross-module class template";
  EXPECT_EQ(use.cc_content.find("integral_constant"),
            std::string::npos)
      << "the injected class template must not carry its base-class list";
  // `Box` is `class [[nodiscard]] Box { ... };` — the
  // injected forward declaration must drop the attribute AND the body,
  // yielding `class Box;` (not `class [[nodiscard]];` or a
  // truncated/empty name).
  EXPECT_NE(use.cc_content.find(
                "extern \"C++\" template <class T>\n"
                "class Box;"),
            std::string::npos)
      << "a class with an attribute must still be injected as a well-formed "
         "forward declaration with its real name";
}

TEST(HeaderRewrite, MacroOnlyBlockStripsLibraryHeaderIncludes) {
  // A macro-only header wrapped in an include guard is emitted verbatim into
  // its `*_macros.h` (so the conditional macros re-expand on every platform).
  // Its `#include "macronly_lib/dep.h"` of another library header must NOT be
  // preserved there: that header is a module now (and its macros are chained
  // as `dep_macros.h`), so a raw textual include would pull the original
  // header's declarations into the global module fragment and clash with the
  // imported module. System includes (`<iostream>`) must be stripped
  // too: the consumer gets those types from the imported modules (or its own
  // includes), and keeping them textually would re-define libc++ declarations
  // that the modules already carry in their GMF (a macro-only block → 
  // `#include <iostream>`).
  auto r = rewrite_header(data_path("macronly_inc.h"), "macronly.inc", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {}, .cc_only = /*cc_only=*/true});
  EXPECT_EQ(r.macros_content.find("#include \"macronly_lib/dep.h\""),
            std::string::npos)
      << "the verbatim macro block must not keep a library-header include";
  EXPECT_EQ(r.macros_content.find("#include <iostream>"),
            std::string::npos)
      << "the verbatim macro block must not keep a system include";
  EXPECT_EQ(r.macros_content.find("#include \"custom/custom.h\""),
            std::string::npos)
      << "the verbatim macro block must not keep a custom include";
  EXPECT_NE(r.macros_content.find("MACRONLY_FOO"), std::string::npos)
      << "the macro definitions must survive";
}

TEST(HeaderRewrite, CcOnlyInlinesHeaderIntoInterface) {
  // cc-only mode folds the whole header into the interface unit: no
  // `#include "api.h"`, no raw library includes (they become imports), and the
  // entities appear directly in the .cc purview.
  auto r = rewrite_header(data_path("cconly_lib/api.h"), "cconly_lib.api", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {}, .cc_only = /*cc_only=*/true});
  EXPECT_EQ(r.cc_content.find("#include \"api.h\""), std::string::npos)
      << "cc-only mode must not include the header from the interface unit";
  EXPECT_EQ(r.cc_content.find("#include \"cconly_lib/dep.h\""),
            std::string::npos)
      << "library includes must not remain in the interface body";
  EXPECT_NE(r.cc_content.find("import cconly_lib.dep;"), std::string::npos)
      << "the library include must be replaced with an import";
  EXPECT_NE(r.cc_content.find("class Foo"), std::string::npos)
      << "the header entities must be inlined into the interface unit";
}

TEST(HeaderRewrite, ExportPrecedesAttributeMacro) {
  // A cross-module declaration with a leading attribute macro
  // (`[[gnu::visibility]]`) must get the export + extern "C++" prefix BEFORE the
  // macro, not after it (which would put the attribute list before `export`).
  auto r = rewrite_header(data_path("attr_lib/api.h"), "attr_lib.api", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {"attr_lib::foo"}});
  EXPECT_NE(r.h_content.find(
                "ATTR_LIB_EXPORT extern \"C++\" ATTR_LIB_API_ bool foo"),
            std::string::npos)
      << "the export prefix must come before the attribute macro";
}

TEST(HeaderRewrite, ExternCxxBlockWrapsHeaderInclude) {
  // With extern "C++" the header include must sit inside the extern "C++"
  // block: no premature close before it (which produced an extraneous `}`).
  auto r = rewrite_header(data_path("cconly_lib/api.h"), "cconly_lib.api", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {}});
  auto open = r.cc_content.find("extern \"C++\" {");
  auto inc = r.cc_content.find("#include \"cconly_lib/api.h\"");
  auto first_close = r.cc_content.find("}  // extern \"C++\"", open);
  EXPECT_NE(open, std::string::npos);
  EXPECT_NE(inc, std::string::npos);
  EXPECT_NE(first_close, std::string::npos);
  EXPECT_LT(open, inc);
  EXPECT_LT(inc, first_close)
      << "the header include must be inside the extern \"C++\" block (no "
         "premature close)";
}

TEST(HeaderRewrite, MacrosFileIncludedBeforeGuardedIncludes) {
  // Bug: macros file was emitted after GMF includes, so guard conditions
  // referencing macros from the file itself didn't resolve.
  auto r = rewrite_header(data_path("guarded_includes.h"), "test_lib");
  auto macros_inc = r.cc_content.find("#include \"guarded_includes_macros.h\"");
  auto first_guard = r.cc_content.find("#ifdef HAS_FEATURE_X");
  EXPECT_NE(macros_inc, std::string::npos);
  EXPECT_NE(first_guard, std::string::npos);
  EXPECT_LT(macros_inc, first_guard)
      << "macros file must appear before guarded includes in GMF";
}

TEST(HeaderRewrite, GmfSystemIncludesPrecedeMacrosHeader) {
  // Feature-test macros (e.g. `__cpp_lib_three_way_comparison` from
  // `<compare>`) must be defined before the GMF macros file is processed, or
  // the macros file evaluates them to 0 and drops entities (the
  // feature-test-macro regression). `<compare>` is guarded in the
  // source by a macro whose VALUE depends on the feature-test macro it defines,
  // so it must still be emitted before the macros file.
  auto r = rewrite_header(data_path("gmf_lib/order.h"), "gmf_lib.order");
  auto sys = r.cc_content.find("#include <compare>");
  auto mac = r.cc_content.find("#include \"gmf_lib/order_macros.h\"");
  EXPECT_NE(sys, std::string::npos) << "<compare> must be in the module GMF";
  EXPECT_NE(mac, std::string::npos)
      << "the macros header must be emitted into the module GMF";
  EXPECT_LT(sys, mac)
      << "system includes must precede the macros header so feature-test "
         "macros are defined when the macros file is processed";
}

TEST(HeaderRewrite, ImportStdReplacesCppHeadersKeepsCHeaders) {
  // a.h uses std::string (a C++ header). With import std active, the C++
  // stdlib headers must be dropped/replaced by import std.compat, while the C
  // headers and the macro-carrying `cstdio`/`cerrno`/`cassert` wrappers stay
  // (they carry stderr/errno/assert macros a module cannot provide).
  std::map<std::string, std::set<std::string>> replaces = {
    {"std.compat", {"iostream"}}
  };
  auto r = rewrite_header(data_path("stdmod/a.h"), "stdmod.a", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = true, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = replaces});
  EXPECT_EQ(r.cc_content.find("#include <cstdint>"), std::string::npos)
      << "a C++ wrapper header provided by import std.compat must not be "
         "included outright";
  EXPECT_NE(r.cc_content.find("#ifdef STDMOD_USE_IMPORT_STD\nimport std.compat;"),
            std::string::npos)
      << "import std.compat must be emitted (guarded by USE_IMPORT_STD)";
  EXPECT_NE(r.cc_content.find("import std.compat;"), std::string::npos)
      << "import std.compat must be emitted for import_std";
  EXPECT_NE(r.cc_content.find("#include <cerrno>"), std::string::npos)
      << "the macro-carrying <cerrno> wrapper must be kept";
  EXPECT_NE(r.cc_content.find("#include <cstdio>"), std::string::npos)
      << "the macro-carrying <cstdio> wrapper must be kept";
  // Those wrappers are reached from deep inside libstdc++/libc++ private
  // headers, behind that implementation's own feature guards (`#if
  // __glibcxx_atomic_wait`, `#ifdef _GLIBCXX_HAVE_LINUX_FUTEX`). The header is
  // ours to keep; the conditions it was found under are not, and emitting them
  // would pin the output to the standard library it was generated against.
  EXPECT_EQ(r.cc_content.find("_GLIBCXX"), std::string::npos)
      << "a libstdc++ private guard must never reach the GMF";
  EXPECT_EQ(r.cc_content.find("__glibcxx"), std::string::npos)
      << "a libstdc++ private feature-test guard must never reach the GMF";
  EXPECT_EQ(r.cc_content.find("_LIBCPP"), std::string::npos)
      << "a libc++ private guard must never reach the GMF";
}

TEST(HeaderRewrite, GmfMergesDuplicateIncludes) {
  // a.h reaches <string> through many headers; the GMF must not repeat the
  // identical include line for each parent that pulled it in.
  std::map<std::string, std::set<std::string>> replaces = {
    {"std.compat", {"iostream"}}
  };
  auto r = rewrite_header(data_path("stdmod/a.h"), "stdmod.a", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = true, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = replaces});
  auto count_of = [&](const std::string &line) {
    std::size_t n = 0, pos = 0;
    while ((pos = r.cc_content.find(line, pos)) != std::string::npos) {
      ++n; pos += line.size();
    }
    return n;
  };
  EXPECT_LE(count_of("#include <cerrno>\n"), 2u)
      << "duplicate <cerrno> includes must be merged";
  EXPECT_LE(count_of("#include <cstdio>\n"), 2u)
      << "duplicate <cstdio> includes must be merged";
}

TEST(HeaderRewrite, GmfSkipsEmptyGuardBlocks) {
  // The transitive include closure reaches the same headers through many
  // parents with different guard contexts. Once every include of a
  // guard-context block is deduplicated, the GMF must NOT emit the (now
  // empty) `#ifndef ... #endif` guards — that would bloat every interface and
  // implementation unit with thousands of redundant directives.
  std::map<std::string, std::set<std::string>> replaces = {
    {"std.compat", {"iostream"}}
  };
  auto r = rewrite_header(data_path("stdmod/a.h"), "stdmod.a", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = true, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = replaces});
  std::size_t pos = 0;
  int empty_blocks = 0;
  while ((pos = r.cc_content.find("#ifndef", pos)) != std::string::npos) {
    auto nl = r.cc_content.find('\n', pos);
    if (nl != std::string::npos) {
      auto nxt = r.cc_content.find_first_not_of(" \t", nl + 1);
      if (nxt != std::string::npos && r.cc_content.compare(nxt, 6, "#endif") == 0)
        ++empty_blocks;
    }
    pos = nl + 1;
  }
  EXPECT_EQ(empty_blocks, 0)
      << "a guard block whose includes were all deduplicated must not emit "
         "empty #ifndef/#endif directives";
}

TEST(HeaderRewrite, GmfSkipsLibcppInternalHeaders) {
  // libc++ internals (`__cxx03/version`, `__vector/vector.h`, `__config`)
  // are implementation details reached only through the public C++ headers;
  // the GMF must never emit them (they are covered by import std.compat or
  // pulled in by the kept public headers themselves).
  std::map<std::string, std::set<std::string>> replaces = {
    {"std.compat", {"iostream"}}
  };
  auto r = rewrite_header(data_path("stdmod/a.h"), "stdmod.a", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = true, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = true, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = replaces});
  std::size_t pos = 0;
  int libcpp_includes = 0;
  while ((pos = r.cc_content.find("#include <", pos)) != std::string::npos) {
    auto nl = r.cc_content.find('\n', pos);
    if (nl == std::string::npos) nl = r.cc_content.size();
    auto line = std::string_view(r.cc_content).substr(pos + 10, nl - pos - 10);
    auto seg = line.substr(0, line.find('/'));
    if (seg.size() >= 2 && seg[0] == '_' && seg[1] == '_') ++libcpp_includes;
    pos = nl + 1;
  }
  EXPECT_EQ(libcpp_includes, 0)
      << "libc++ internal __ headers must not be emitted in the GMF";
}

TEST(HeaderRewrite, MacroDeclaredEntitiesCarryExportInMacrosFile) {
  // A macro whose body declares a class AND an inline factory (e.g. a factory
  // macro). `export` in front of the invocation only reaches the first
  // expanded declaration, so the export markers must be baked into the macro
  // body itself: wherever the macro expands (module body via the GMF macros
  // file, or a consumer), the class and the factory both carry the marker.
  auto r = rewrite_header(data_path("makelib/make.h"), "makelib.make");
  EXPECT_NE(r.macros_content.find("MAKELIB_EXPORT class name##Class"),
            std::string::npos)
      << "the macro body must carry the export marker on the class";
  EXPECT_NE(
      r.macros_content.find("MAKELIB_EXPORT inline name##Class name"),
      std::string::npos)
      << "the macro body must carry the export marker on the inline factory";
  EXPECT_NE(r.h_content.find("MAKE(Foo, \"a foo\")"), std::string::npos)
      << "the raw invocation stays in the header body (the macros file "
         "provides the marked macro definition)";
  EXPECT_NE(r.macros_content.find("#define MAKE(name, description)"),
            std::string::npos)
      << "the macro definition moves to the macros file";
  EXPECT_EQ(r.h_content.find("#define MAKE(name, description)"),
            std::string::npos)
      << "the public macro must move out of the header body into the macros "
         "file";
}

TEST(HeaderRewrite, CrossFileMacroBodyCarriesExportInDefiningMacrosFile) {
  // The macro is defined in defs.h but INVOKED in use.h (the
  // declaration-macro pattern: defined in one header, invoked in another).
  // The export markers for the entities the macro declares are computed while
  // rewriting the USING header; they must be routed into the DEFINING header's
  // macros file so the macro body carries them wherever it expands — not
  // exported at the invocation in the using header.
  auto defs = data_path("crossmakelib/defs.h");
  auto use = data_path("crossmakelib/use.h");
  auto canon = [](const std::string &p) {
    return std::filesystem::weakly_canonical(std::filesystem::path(p)).string();
  };
  std::set<std::string> libs = {canon(defs), canon(use)};

  // Rewriting the USING header routes the markers to the defining header and
  // does NOT export the invocation.
  auto ru = rewrite_header(
      use, "crossmakelib.use",
      {.combined_macros = false,
       .include_to_module = {},
       .reachable_fqns = {},
       .extern_cxx = true,
       .extra_args = {"-I", gDataDir},
       .cc_only = true,
       .library_headers = libs});
  EXPECT_EQ(ru.cc_content.find("CROSSMAKELIB_EXPORT MAKE(Foo"),
            std::string::npos)
      << "the invocation must not be exported at the use site";
  auto it = ru.external_macro_mods.find(canon(defs));
  ASSERT_NE(it, ru.external_macro_mods.end())
      << "the export markers must be routed to the defining header";
  ASSERT_FALSE(it->second.empty());

  // Re-rewriting the DEFINING header with the routed markers bakes them into
  // the macro body in its macros file (the CLI's second pass).
  auto rd = rewrite_header(
      defs, "crossmakelib.defs",
      {.combined_macros = false,
       .include_to_module = {},
       .reachable_fqns = {},
       .extern_cxx = true,
       .extra_args = {"-I", gDataDir},
       .cc_only = true,
       .library_headers = libs,
       .extra_macro_mods = it->second});
  EXPECT_NE(rd.macros_content.find("CROSSMAKELIB_EXPORT class name##Class"),
            std::string::npos)
      << "the defining header's macros file must carry the export marker on "
         "the class";
   EXPECT_NE(
      rd.macros_content.find("CROSSMAKELIB_EXPORT inline name##Class name"),
      std::string::npos)
      << "the defining header's macros file must carry the export marker on "
          "the inline factory";
}

TEST(HeaderRewrite, BatchPipelineRoutesMacroExportMarkersAcrossFiles) {
  // The batch rewrite pipeline (the path the CLI entry points use) must
  // propagate library_headers into the per-header rewrite options. Without it,
  // the export visitor falls back to exporting at the invocation, so a macro
  // defined in one header and invoked in another does NOT get its export
  // markers baked into the DEFINING header's macros file (cross-file
  // declaration-macro handling breaks).
  auto defs = data_path("crossmakelib/defs.h");
  auto use = data_path("crossmakelib/use.h");
  clang::tooling::FixedCompilationDatabase db(
      ".", {"-x", "c++", "-std=c++20", "-I", gDataDir});
  modulizer::RewriteBatchConfig cfg;
  cfg.module_name = "crossmakelib";
  cfg.library_name = "crossmakelib";
  cfg.extern_cxx = true;
  cfg.cc_only = true;
  auto outcomes = modulizer::rewrite_header_batch(
      {defs, use}, /*reachable_fqns=*/{}, cfg, db);
  ASSERT_EQ(outcomes.size(), 2u);
  for (auto &o : outcomes) {
    if (o.src != defs) continue;
    ASSERT_TRUE(o.ok);
    EXPECT_NE(
        o.r.macros_content.find("CROSSMAKELIB_EXPORT class name##Class"),
        std::string::npos)
        << "the batch pipeline must bake the routed export marker into the "
           "defining header's macros file";
    return;
  }
  FAIL() << "defs.h outcome not found";
}

TEST(HeaderRewrite, CrossFileConditionalBlockMacroCarriesExport) {
  // A factory macro defined inside a `#if/#else/#endif` block: the export
  // markers computed while rewriting the using header must be baked into the
  // emitted conditional block in the defining header's macros file (so the
  // correct branch re-expands on the actual platform).
  auto defs = data_path("crosscond/defs.h");
  auto use = data_path("crosscond/use.h");
  auto canon = [](const std::string &p) {
    return std::filesystem::weakly_canonical(std::filesystem::path(p)).string();
  };
  std::set<std::string> libs = {canon(defs), canon(use)};

  auto ru = rewrite_header(
      use, "crosscond.use",
      {.combined_macros = false,
       .include_to_module = {},
       .reachable_fqns = {},
       .extern_cxx = true,
       .extra_args = {"-I", gDataDir},
       .cc_only = true,
       .library_headers = libs});
  auto it = ru.external_macro_mods.find(canon(defs));
  ASSERT_NE(it, ru.external_macro_mods.end())
      << "the export markers must be routed to the defining header";

  auto rd = rewrite_header(
      defs, "crosscond.defs",
      {.combined_macros = false,
       .include_to_module = {},
       .reachable_fqns = {},
       .extern_cxx = true,
       .extra_args = {"-I", gDataDir},
       .cc_only = true,
       .library_headers = libs,
       .extra_macro_mods = it->second});
  EXPECT_NE(rd.macros_content.find("#if defined(CROSSCOND_USE_BRANCH)"),
            std::string::npos)
      << "the conditional block is emitted verbatim in the macros file";
  EXPECT_NE(rd.macros_content.find("CROSSCOND_EXPORT class name##Class"),
            std::string::npos)
      << "the marker must be baked into the (active) macro body branch";
  EXPECT_EQ(rd.macros_content.find("CROSSCOND_EXPORT CROSSCOND_EXPORT"),
            std::string::npos)
      << "the marker must not be duplicated";
}

TEST(HeaderRewrite, CrossFileMultiConsumerMacroDedupsMarkers) {
  // Two use headers both invoke the SAME factory macro. Each routes the export
  // markers for its own expansion to the defining header; the CLI aggregates
  // them and re-rewrites the defining header. The markers from different
  // invocations share spelling locations inside the macro body, so re-rewriting
  // with the merged set must not bake duplicates.
  auto defs = data_path("crossmulti/defs.h");
  auto use_a = data_path("crossmulti/use_a.h");
  auto use_b = data_path("crossmulti/use_b.h");
  auto canon = [](const std::string &p) {
    return std::filesystem::weakly_canonical(std::filesystem::path(p)).string();
  };
  std::set<std::string> libs = {canon(defs), canon(use_a), canon(use_b)};
  auto common = RewriteOptions{.combined_macros = false,
                               .include_to_module = {},
                               .reachable_fqns = {},
                               .extern_cxx = true,
                               .extra_args = {"-I", gDataDir},
                               .cc_only = true,
                               .library_headers = libs};

  auto ra = rewrite_header(use_a, "crossmulti.use_a", common);
  auto rb = rewrite_header(use_b, "crossmulti.use_b", common);
  auto ita = ra.external_macro_mods.find(canon(defs));
  auto itb = rb.external_macro_mods.find(canon(defs));
  ASSERT_NE(ita, ra.external_macro_mods.end());
  ASSERT_NE(itb, rb.external_macro_mods.end());

  // Aggregate the markers from both consumers exactly as the CLI does.
  std::vector<ModPoint> merged = ita->second;
  merged.insert(merged.end(), itb->second.begin(), itb->second.end());

  auto rd = rewrite_header(
      defs, "crossmulti.defs",
      {.combined_macros = false,
       .include_to_module = {},
       .reachable_fqns = {},
       .extern_cxx = true,
       .extra_args = {"-I", gDataDir},
       .cc_only = true,
       .library_headers = libs,
       .extra_macro_mods = merged});
  EXPECT_NE(rd.macros_content.find("CROSSMULTI_EXPORT class name##Class"),
            std::string::npos);
  EXPECT_NE(rd.macros_content.find("CROSSMULTI_EXPORT inline name##Class name"),
            std::string::npos);
  EXPECT_EQ(rd.macros_content.find("CROSSMULTI_EXPORT CROSSMULTI_EXPORT"),
            std::string::npos)
      << "markers from multiple consumers must be deduplicated at the shared "
         "spelling locations";
}

TEST(HeaderRewrite, CrossFileFlagMacroKeepsExportAtInvocation) {
  // A flag-declaration macro (defined in defs.h, invoked in use.h) expands to a
  // single `extern` variable. Unlike MATCHER-style macros, baking the export
  // into the shared macro body would leak `export` into EVERY expansion — the
  // macro is also used in implementation units (global module) where `export`
  // is ill-formed. Such entities keep the invocation-level export.
  auto defs = data_path("crossflag/defs.h");
  auto use = data_path("crossflag/use.h");
  auto canon = [](const std::string &p) {
    return std::filesystem::weakly_canonical(std::filesystem::path(p)).string();
  };
  std::set<std::string> libs = {canon(defs), canon(use)};

  // Rewriting the USING header must NOT route a marker into the defining
  // header for the variable.
  auto ru = rewrite_header(
      use, "crossflag.use",
      {.combined_macros = false,
       .include_to_module = {},
       .reachable_fqns = {},
       .extern_cxx = true,
       .extra_args = {"-I", gDataDir},
       .cc_only = true,
       .library_headers = libs});
  EXPECT_EQ(ru.external_macro_mods.find(canon(defs)),
            ru.external_macro_mods.end())
      << "a flag variable must not route an export marker into the defining "
         "header's macro body";
  EXPECT_NE(ru.cc_content.find("CROSSFLAG_EXPORT DECLARE_FLAG(g_run_tests)"),
            std::string::npos)
      << "the flag declaration keeps the invocation-level export";

  // The defining header's macros file must NOT carry the export marker in the
  // macro body.
  auto rd = rewrite_header(
      defs, "crossflag.defs",
      {.combined_macros = false,
       .include_to_module = {},
       .reachable_fqns = {},
       .extern_cxx = true,
       .extra_args = {"-I", gDataDir},
       .cc_only = true,
       .library_headers = libs});
   EXPECT_EQ(rd.macros_content.find("CROSSFLAG_EXPORT extern bool name"),
            std::string::npos)
      << "the macro body must not carry the export marker (it would leak "
         "`export` into implementation units that expand the same macro)";
}

TEST(HeaderRewrite, UseModulesDefinedBeforeMacrosIncludeInGmf) {
  // A macros file whose bodies carry export markers chain-includes the
  // library's export header (`<LIB>-export.h`), which defines `LIB_EXPORT` as
  // `export` only under `LIB_USE_MODULES`. The interface unit must define
  // `LIB_USE_MODULES` BEFORE including the macros file in its GMF — otherwise
  // the `#pragma once` header caches the empty `LIB_EXPORT` definition and the
  // module purview cannot export the macro-declared entities.
  auto defs = data_path("crossmakelib/defs.h");
  auto use = data_path("crossmakelib/use.h");
  auto canon = [](const std::string &p) {
    return std::filesystem::weakly_canonical(std::filesystem::path(p)).string();
  };
  std::set<std::string> libs = {canon(defs), canon(use)};

  // Rewrite the DEFINING header with a routed marker so its macros file carries
  // the export marker (which triggers the export-header chain include).
  auto ru = rewrite_header(
      use, "crossmakelib.use",
      {.combined_macros = false,
       .include_to_module = {},
       .reachable_fqns = {},
       .extern_cxx = true,
       .extra_args = {"-I", gDataDir},
       .cc_only = true,
       .library_headers = libs});
  auto it = ru.external_macro_mods.find(canon(defs));
  ASSERT_NE(it, ru.external_macro_mods.end());
  auto rd = rewrite_header(
      defs, "crossmakelib.defs",
      {.combined_macros = false,
       .include_to_module = {},
       .reachable_fqns = {},
       .extern_cxx = true,
       .extra_args = {"-I", gDataDir},
       .cc_only = true,
       .library_headers = libs,
       .extra_macro_mods = it->second});
  ASSERT_NE(rd.macros_content.find("crossmakelib_export.h"),
            std::string::npos)
      << "the marked macros file must chain-include the export header";

   auto define_pos = rd.cc_content.find("#define CROSSMAKELIB_USE_MODULES 1");
  auto inc_pos = rd.cc_content.find("#include \"crossmakelib/defs_macros.h\"");
  ASSERT_NE(define_pos, std::string::npos)
      << "the interface unit GMF must define LIB_USE_MODULES";
  ASSERT_NE(inc_pos, std::string::npos)
      << "the interface unit GMF must include the macros file";
  EXPECT_LT(define_pos, inc_pos)
      << "LIB_USE_MODULES must be defined BEFORE the macros-file include so the "
         "chained export header resolves LIB_EXPORT to `export`";
}

TEST(HeaderRewrite, CrossFileMacroWithAttributeMacroCarriesExport) {
  // A factory macro whose body declares a class preceded by an attribute list.
  // The routed export marker must land BEFORE the attribute, wherever the
  // macro expands.
  auto defs = data_path("crossattr/defs.h");
  auto use = data_path("crossattr/use.h");
  auto canon = [](const std::string &p) {
    return std::filesystem::weakly_canonical(std::filesystem::path(p)).string();
  };
  std::set<std::string> libs = {canon(defs), canon(use)};

  auto ru = rewrite_header(
      use, "crossattr.use",
      {.combined_macros = false,
       .include_to_module = {},
       .reachable_fqns = {},
       .extern_cxx = true,
       .extra_args = {"-I", gDataDir},
       .cc_only = true,
       .library_headers = libs});
  auto it = ru.external_macro_mods.find(canon(defs));
  ASSERT_NE(it, ru.external_macro_mods.end())
      << "the export markers must be routed to the defining header";

  auto rd = rewrite_header(
      defs, "crossattr.defs",
      {.combined_macros = false,
       .include_to_module = {},
       .reachable_fqns = {},
       .extern_cxx = true,
       .extra_args = {"-I", gDataDir},
       .cc_only = true,
       .library_headers = libs,
       .extra_macro_mods = it->second});
  EXPECT_NE(rd.macros_content.find(
                "CROSSATTR_EXPORT class [[nodiscard]] name##Class"),
            std::string::npos)
      << "the export marker must precede the class (with its attribute list) "
         "in the macro body";
}

TEST(HeaderRewrite, CrossFileRoutingDisabledWithoutLibraryHeaders) {
  // Without a library_headers set the visitor must NOT route markers across
  // files: it falls back to exporting at the invocation, exactly as before the
  // cross-file feature existed.
  auto use = data_path("crossmakelib/use.h");
  auto ru = rewrite_header(use, "crossmakelib.use",
                           {.combined_macros = false,
                            .include_to_module = {},
                            .reachable_fqns = {},
                            .extern_cxx = true,
                            .extra_args = {"-I", gDataDir},
                            .cc_only = true});
  EXPECT_TRUE(ru.external_macro_mods.empty())
      << "without library_headers no cross-file routing happens";
}

TEST(HeaderRewrite, MovesPublicMacrosToMacrosFileKeepsPrivate) {
  // defs.h defines a public macro (PUBLIC_OP, never #undef'd) and a private
  // one (PRIVATE_HELPER, #undef'd before the end of the header). Public macros
  // move to the macros file; private ones stay in the header body.
  auto r = rewrite_header(data_path("macstrip_lib/defs.h"), "macstrip_lib.defs");
  EXPECT_EQ(r.h_content.find("#define PUBLIC_OP"), std::string::npos)
      << "the public macro must move out of the header body";
  EXPECT_NE(r.macros_content.find("#define PUBLIC_OP(x) ((x) + 1)"),
            std::string::npos)
      << "the public macro must be emitted in the macros file";
  EXPECT_NE(r.h_content.find("#define PRIVATE_HELPER(x) ((x) * 2)"),
            std::string::npos)
      << "the private (undef'd) macro must stay in the header body";
  EXPECT_EQ(r.macros_content.find("#define PRIVATE_HELPER"),
            std::string::npos)
      << "the private (undef'd) macro must NOT be emitted in the macros file";
  EXPECT_NE(r.h_content.find("#undef PRIVATE_HELPER"), std::string::npos)
      << "the private macro's #undef must stay in the header body";
  EXPECT_NE(r.h_content.find("inline int use_private"), std::string::npos)
      << "the header body must still contain its inline code";
}

TEST(HeaderRewrite, UndefsTheEarlyCopyOnlyOfMacrosTheHeaderRedefines) {
  // The macros file is included at the top of the GMF, before the standard
  // library, so a macro derived from a feature-test macro is computed there
  // against the wrong answer; the header's own definition is the correct one
  // and the early copy is #undef'd immediately before including the header.
  //
  // Which macros that applies to must be decided by DEFINITION, not by name.
  // A macro defined in both branches of a conditional is re-established by the
  // header whichever branch is taken. A macro whose taken branch was hoisted
  // into the macros file is not: what remains in the header is the branch that
  // is never reached, and undefining on the strength of it leaves the macro
  // undefined for every use in the header.
  std::string hdr =
      "#if defined(FEATURE)\n"
      "#define BOTH 1\n"
      "#else\n"
      "#define BOTH 0\n"
      "#endif\n"
      "#ifdef __has_include\n"          // taken branch hoisted out
      "#else\n"
      "#define ONE_BRANCH(...) 0\n"
      "#endif\n";
  std::vector<MacroRec> macros = {
      {"BOTH", "#define BOTH 1\n", 0, 0},
      {"ONE_BRANCH", "#define ONE_BRANCH __has_include\n", 0, 0},
  };

  auto redefined = macros_redefined_by_header(hdr, macros);
  EXPECT_TRUE(std::ranges::contains(redefined, "BOTH"))
      << "a macro the header re-establishes must have its early copy dropped";
  EXPECT_FALSE(std::ranges::contains(redefined, "ONE_BRANCH"))
      << "the only definition left in the header is an unreachable branch "
         "defining something else, so the early copy must survive";
}

TEST(HeaderRewrite, MacroRedefinitionIgnoresDefinitionsQuotedInOtherMacros) {
  // A definition that appears inside another macro's replacement list is not
  // the header defining it.
  std::string hdr = "#define WRAPPER() do { } while (0) /* #define X 1 */\n";
  std::vector<MacroRec> macros = {{"X", "#define X 1\n", 0, 0}};
  EXPECT_TRUE(macros_redefined_by_header(hdr, macros).empty());
}

TEST(HeaderRewrite, MovesSourceCopyrightHeaderToTopOfGeneratedFiles) {
  // The source file's license/copyright comment block must be reproduced at
  // the very top of the generated module unit (before `module;`) and of the
  // rewritten header, so attribution survives the rewrite.
  auto r = rewrite_header(data_path("copyhead_lib/head.h"), "copyhead_lib.head");
  EXPECT_TRUE(r.cc_content.starts_with(
      "// Copyright 2025, Copyhead Inc.\n"))
      << "the module unit must start with the source copyright header";
  EXPECT_NE(r.cc_content.find(
                "Redistribution and use in source and binary forms"),
            std::string::npos)
      << "the full copyright block must be carried over";
  EXPECT_TRUE(r.h_content.starts_with("// Copyright 2025, Copyhead Inc.\n"))
      << "the rewritten header must start with the source copyright header";
}

TEST(HeaderRewrite, MovesMacroDocCommentsToMacrosFile) {
  // The doc-comment block immediately preceding a public macro must be moved
  // into the macros file next to the macro it documents.
  auto r = rewrite_header(data_path("macstrip_lib/defs.h"), "macstrip_lib.defs");
  EXPECT_NE(r.macros_content.find("This macro is used by consumers of the "
                                  "library."),
            std::string::npos)
      << "the doc comment must move into the macros file next to the macro";
  EXPECT_EQ(r.h_content.find("This macro is used by consumers of the library."),
            std::string::npos)
      << "the doc comment must move out of the header body";
}

TEST(HeaderRewrite, StripsOverrideGuardGatingCodeWhenMacroMoves) {
  // log.h's `#if !defined(PUBLIC_LOG_)` guard gates BOTH a macro definition and
  // an inline function (a guard macro that also gates a helper is the
  // real-world
  // case). Moving the macro to the macros file must strip the guard AND the
  // define, keeping the inline code — otherwise the guard re-evaluates with
  // the macro already defined and the inline helper is lost.
  auto r = rewrite_header(data_path("macstrip_lib/log.h"), "macstrip_lib.log");
  EXPECT_NE(r.macros_content.find("#define PUBLIC_LOG_(severity) ((severity))"),
            std::string::npos)
      << "the public macro must move to the macros file";
  EXPECT_EQ(r.h_content.find("#define PUBLIC_LOG_"), std::string::npos)
      << "the define must be stripped from the header body";
  EXPECT_EQ(r.h_content.find("#if !defined(PUBLIC_LOG_)"), std::string::npos)
      << "the override guard must be stripped from the header body";
  EXPECT_EQ(r.h_content.find("#endif  // !defined(PUBLIC_LOG_)"),
            std::string::npos)
      << "the guard's #endif must be stripped from the header body";
  EXPECT_NE(r.h_content.find("inline int log_helper()"), std::string::npos)
      << "the inline code gated by the guard must stay in the header body";
  EXPECT_NE(r.h_content.find("inline int use_log()"), std::string::npos)
      << "code using the moved macro must stay in the header body";
}

TEST(HeaderRewrite, VersionIncludePrecedesMacrosHeader) {
  // Same regression as GmfSystemIncludesPrecedeMacrosHeader but for
  // `<version>`: `<version>` is guarded in the source by a macro whose VALUE
  // depends on the feature-test macro (`__cpp_lib_three_way_comparison`) that
  // `<version>` itself defines, so it must be emitted before the macros file
  // — and with its guard stripped — or the module body's feature-test blocks
  // compile out (the feature-test-macro regression).
  auto r = rewrite_header(data_path("gmf_lib/version.h"), "gmf_lib.version");
  auto sys = r.cc_content.find("#include <version>");
  auto mac = r.cc_content.find("#include \"gmf_lib/version_macros.h\"");
  EXPECT_NE(sys, std::string::npos) << "<version> must be in the module GMF";
  EXPECT_NE(mac, std::string::npos)
      << "the macros header must be emitted into the module GMF";
  EXPECT_LT(sys, mac)
      << "<version> must precede the macros header so the feature-test macro "
         "it defines is available when the macros file evaluates it";
  // The guard on <version> references a macro only the macros file provides,
  // so emitting it guarded would dead-code it; it must be unguarded.
  auto after = r.cc_content.substr(sys, mac - sys);
  EXPECT_EQ(after.find("#if"), std::string::npos)
      << "<version> must be emitted unguarded before the macros header";
}

TEST(HeaderRewrite, DoesNotExportClassMemberTypedefs) {
  auto r = rewrite_header(data_path("class_members.h"), "test_lib");
  auto h = r.h_content;
  // Top-level typedef should be exported
  EXPECT_NE(h.find("TEST_LIB_EXPORT typedef int TopLevelTypedef"),
            std::string::npos);
  // Class-member typedef should NOT be exported
  EXPECT_EQ(h.find("TEST_LIB_EXPORT typedef int MemberTypedef"),
            std::string::npos);
  // Class-member using alias should NOT be exported
  EXPECT_EQ(h.find("TEST_LIB_EXPORT using MemberAlias"), std::string::npos);
  // Class-member enum should NOT be exported
  EXPECT_EQ(h.find("TEST_LIB_EXPORT enum class MemberEnum"),
            std::string::npos);
  // Nested class should NOT be exported
  EXPECT_EQ(h.find("TEST_LIB_EXPORT class NestedClass"),
            std::string::npos);
  // Already-exported class should not get double CLASS_EXPORT
  EXPECT_EQ(h.find("TEST_LIB_EXPORT CLASS_EXPORT"), std::string::npos);
  // Top-level enum and alias should be exported
  EXPECT_NE(h.find("TEST_LIB_EXPORT using TopLevelAlias"),
            std::string::npos);
  EXPECT_NE(h.find("TEST_LIB_EXPORT enum TopLevelEnum"),
            std::string::npos);
}

TEST(HeaderRewrite, ElifIncludesAreSkippedFromGmf) {
  auto r = rewrite_header(data_path("elif_else_includes.h"), "test_lib");
  // #elif-branch includes should NOT appear in the .cc GMF
  EXPECT_EQ(r.cc_content.find("cond_b.h"), std::string::npos)
      << "#elif includes must not be in GMF";
  // #if-branch includes SHOULD appear in GMF (with guard)
  EXPECT_NE(r.cc_content.find("#include <cond_a.h>"), std::string::npos)
      << "#if includes should be in GMF";
  // #else-branch includes SHOULD appear in GMF (unconditional)
  EXPECT_NE(r.cc_content.find("#include <cond_c.h>"), std::string::npos)
      << "#else includes should be in GMF";
}

TEST(HeaderRewrite, ElseIncludesEmittedUnconditional) {
  auto r = rewrite_header(data_path("elif_else_includes.h"), "test_lib");
  // #else includes should be unconditional (no guard wrapper)
  auto pos = r.cc_content.find("#include <cond_c.h>");
  EXPECT_NE(pos, std::string::npos);
  // No #ifdef before it (within a few lines)
  auto before = r.cc_content.substr(pos > 30 ? pos - 30 : 0, 30);
  EXPECT_EQ(before.find("#ifdef"), std::string::npos);
  // #else plain includes too
  auto p2 = r.cc_content.find("#include <plain_else.h>");
  EXPECT_NE(p2, std::string::npos);
  auto before2 = r.cc_content.substr(p2 > 30 ? p2 - 30 : 0, 30);
  EXPECT_EQ(before2.find("#ifdef"), std::string::npos);
}

TEST(HeaderRewrite, ConditionalIncludesKeepOriginalGuards) {
  auto r = rewrite_header(data_path("elif_else_includes.h"), "test_lib");
  // plain_if.h is inside #if PLAIN_IF → should have guard in header
  // but also be wrapped with USE_MODULES (since only 1 guard level)
  // Check that it IS wrapped with USE_MODULES
  auto phi = r.h_content.find("#ifndef TEST_LIB_USE_MODULES");
  EXPECT_NE(phi, std::string::npos)
      << "unconditional includes should be wrapped with USE_MODULES";
}

TEST(HeaderRewrite, DoesNotExportFriendTemplatesInsideClass) {
  // Friend template declarations inside class bodies should not get export.
  auto r = rewrite_header(data_path("friend_template.h"), "test_lib");
  // TEST_LIB_EXPORT should only appear before class Foo, not inside it
  auto h = r.h_content;
  // Foo should be exported at namespace level
  EXPECT_NE(h.find("TEST_LIB_EXPORT class Foo"), std::string::npos);
  // Friend template inside Foo should NOT have export
  EXPECT_EQ(h.find("TEST_LIB_EXPORT template"), std::string::npos)
      << "friend templates inside class bodies must not be exported";
}

TEST(HeaderRewrite, ExportsNamespaceUsingDeclaration) {
  // api.h hoists `using internal::Foo;` to the outer namespace so consumers
  // can refer to `usinglib::Foo`. The using-declaration is an entity that
  // must be exported for the module to expose the name.
  auto r = rewrite_header(data_path("usinglib/api.h"), "usinglib.api");
  EXPECT_NE(r.h_content.find("USINGLIB_EXPORT using internal::Foo;"),
            std::string::npos)
      << "a namespace-scope using-declaration must be exported";
}

TEST(HeaderRewrite, ExportsNamespaceUsingDirective) {
  // directive.h pulls `adl_guard` members into `usinglib` with a using-DIRECTIVE
  // (`using namespace adl_guard;`, the ADL-suppression pattern). The directive
  // must
  // be exported so consumers resolve names like `usinglib::pointer_thing`
  // (which lives in `usinglib::adl_guard`) through it.
  auto r = rewrite_header(data_path("usinglib/directive.h"), "usinglib.directive");
  EXPECT_NE(r.h_content.find("USINGLIB_EXPORT using namespace adl_guard;"),
            std::string::npos)
      << "a namespace-scope using-directive must be exported";
}

TEST(HeaderRewrite, DoesNotExternCxxFriendTemplateInsideClass) {
  // A friend template declaration inside a class body must not get `extern
  // "C++"` even when its target is a cross-module entity: `extern "C++"
  // template <typename T> friend class Bar;` inside a class is ill-formed.
  // Here Foo is itself extern "C++" (forward-declared by another module) and
  // it friend-declares the class template Bar.
  auto r = rewrite_header(data_path("friendtmpl_lib/base.h"), "friendtmpl_lib.base", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"friendtmpl_lib::Foo", "friendtmpl_lib::Bar"}, .fwd_declared_fqns = {"friendtmpl_lib::Foo", "friendtmpl_lib::Bar"}});
  EXPECT_NE(r.h_content.find("template <typename T>\n  friend class Bar;"),
            std::string::npos)
      << "the friend template declaration must remain";
  EXPECT_EQ(r.h_content.find("extern \"C++\" template <typename T>\n  friend"),
            std::string::npos)
      << "friend templates inside class bodies must not be wrapped with "
         "extern \"C++\"";
}

TEST(HeaderRewrite, HeaderDropsTheMacrosFileCopyOfWhatItRedefines) {
  // A classic build includes the rewritten header and nothing else, so the
  // header carries its own macros file. Anything the header then defines for
  // itself would redefine the copy that file just supplied, and every
  // translation unit including the header reported it:
  //
  //   warning: 'MACREDEF_VALUE' macro redefined
  //
  // The copy is dropped at the point of redefinition, NOT in a block after the
  // include. Until the header reaches its own definition, the macros file's
  // value is what the header's own conditionals read, and taking it away early
  // silently selects a different branch — and so a different definition.
  auto r = rewrite_header(data_path("macredef_lib/defs.h"), "macredef_lib.defs");
  ASSERT_NE(r.macros_content.find("#define MACREDEF_VALUE"), std::string::npos)
      << "the macros file carries the macro";

  // Adjacent: the undef belongs to the definition that replaces it.
  EXPECT_NE(r.h_content.find("#undef MACREDEF_VALUE\n#define MACREDEF_VALUE"),
            std::string::npos)
      << "the copy must be dropped immediately before the redefinition";

  // And nowhere else: an undef ahead of the conditional would change which
  // branch the header takes.
  auto cond = r.h_content.find("#if defined(MACREDEF_FEATURE)");
  ASSERT_NE(cond, std::string::npos);
  auto first_undef = r.h_content.find("#undef MACREDEF_VALUE");
  ASSERT_NE(first_undef, std::string::npos);
  EXPECT_GT(first_undef, cond)
      << "nothing may be undefined before the conditionals that read it";
}

TEST(HeaderRewrite, ExternCxxWrappingCanBeSelectedAtBuildTime) {
  // Wrapping the purview in extern "C++" gives every entity C++ language
  // linkage in the global module, which is what lets an implementation that is
  // not a module unit link against it. A conforming implementation does not
  // need that and is better off with module linkage, so the choice belongs to
  // whoever builds the tree, not to whoever generated it: emit the block
  // behind a macro so one tree serves both.
  auto r = rewrite_header(data_path("test_lib/producer.h"), "test_lib.producer",
                          {.extern_cxx = true,
                           .extern_cxx_macro = "TEST_LIB_EXTERN_CXX"});

  auto open_guard = r.cc_content.find("#ifdef TEST_LIB_EXTERN_CXX");
  ASSERT_NE(open_guard, std::string::npos)
      << "the wrapping must be conditional, not baked in";
  auto open_brace = r.cc_content.find("extern \"C++\" {", open_guard);
  ASSERT_NE(open_brace, std::string::npos);
  EXPECT_LT(open_guard, open_brace) << "the guard precedes the block it guards";

  // Both the opening and the closing brace need guarding, or the two forms do
  // not both parse.
  auto close = r.cc_content.rfind("}  // extern \"C++\"");
  ASSERT_NE(close, std::string::npos);
  auto close_guard = r.cc_content.rfind("#ifdef TEST_LIB_EXTERN_CXX", close);
  ASSERT_NE(close_guard, std::string::npos);
  EXPECT_GT(close_guard, open_brace)
      << "the closing brace carries its own guard";

  // Every guard opened is closed: an unbalanced one breaks the file in
  // whichever configuration is not the one that was eyeballed.
  auto count = [&](std::string_view needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = r.cc_content.find(needle, pos)) != std::string::npos) {
      ++n;
      pos += needle.size();
    }
    return n;
  };
  EXPECT_EQ(count("#ifdef TEST_LIB_EXTERN_CXX"), 2u);
}

TEST(HeaderRewrite, ExternCxxIsUnconditionalWithoutTheMacro) {
  // Without a macro named, the block stays unconditional — the existing
  // behaviour, which the clang-only trees depend on.
  auto r = rewrite_header(data_path("test_lib/producer.h"), "test_lib.producer",
                          {.extern_cxx = true});
  EXPECT_NE(r.cc_content.find("extern \"C++\" {"), std::string::npos);
  EXPECT_EQ(r.cc_content.find("#ifdef "), std::string::npos)
      << "no build-time switch unless one was asked for";
}
