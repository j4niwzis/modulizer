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
// FullRewrite suite
TEST(FullRewrite, SourceDirHeaderImportsLibraryDerivedModule) {
  // An implementation unit that includes `src/header.h` (a same-library header
  // under a source directory, e.g. `src/mylib-internal-inl.h`) must import the
  // module derived from the LIBRARY name (`mylib.inl`), not from the first
  // path segment (`src.inl`). The cross-library auto-import must not treat
  // `src/` as a library root when the header belongs to the current library.
  auto r = rewrite_source(
      data_path("srcdir_lib/impl.cc"), "mylib.impl", {},
      {"-I", gDataDir + "/srcdir_lib"}, false, /*extern_cxx=*/true, {},
      {}, {"mylib.inl"});
  EXPECT_NE(r.content.find("import mylib.inl;"), std::string::npos)
      << "the same-library source-dir header must be imported under its "
         "library-derived module name";
  EXPECT_EQ(r.content.find("import src.inl;"), std::string::npos)
      << "the first path segment (`src`) must not be mistaken for a library "
         "root";
}

TEST(FullRewrite, WrapperExportsOnlyPublicConsumerReachableInternals) {
  // The generated wrapper re-exports public API plus the internal entities
  // reachable from PUBLIC consumers and macros; entities reachable only from
  // INTERNAL consumers (the library's own tests/main/impls) are exported from
  // their defining modules but must NOT appear in the wrapper facade.
  EntityModel model;
  model.items.push_back({EntityItem::kClass, "SecretInternal",
                         {"twocons_lib", "internal"}});
  model.items.push_back({EntityItem::kClass, "ExposedInternal",
                         {"twocons_lib", "internal"}});
  model.items.push_back({EntityItem::kClass, "Public", {"twocons_lib"}});
  std::map<std::string, std::vector<std::string>> public_reachable = {
      {"producer.h", {"twocons_lib::internal::ExposedInternal"}}};
  auto wrapper_reachable = modulizer::wrapper_reachable_fqns(
      /*manual_fqns=*/{}, /*macro_reachable=*/{}, /*alias_reachable=*/{},
      public_reachable);
  auto out = generate_import_wrapper_cc(
      "twocons_lib", "twocons_lib.umbrella", model, kDefaultInternalFilter,
      wrapper_reachable);
  EXPECT_NE(out.find("using ::twocons_lib::internal::ExposedInternal;"),
            std::string::npos)
      << "an internal entity reachable from a public consumer must be "
         "re-exported by the wrapper";
  EXPECT_EQ(out.find("SecretInternal"), std::string::npos)
      << "an internal entity reachable only from internal consumers must not "
         "be re-exported by the wrapper";
  EXPECT_NE(out.find("using ::twocons_lib::Public;"), std::string::npos)
      << "public API entities must always be re-exported";
}

TEST(FullRewrite, InternalAliasUsedViaDependentNameIsExported) {
  // producer.h defines an alias template in an internal namespace, referenced
  // by another module through `typename Foo<...>::type`. The alias must
  // be exported from the producer module so sibling modules can use it.
  auto producer = data_path("alias_lib/producer.h");
  auto consumer = data_path("alias_lib/consumer.h");
  std::vector<std::string> headers = {producer, consumer};
  std::vector<std::string> extra_args = {"-I", gDataDir};
  auto reachable = trace_consumer_reachability(headers, "alias_lib", extra_args);
  auto it = reachable.find(producer);
  ASSERT_NE(it, reachable.end())
      << "consumer must reach the producer's internal entities";
  auto r = rewrite_header(producer, "alias_lib.producer", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = it->second, .extern_cxx = /*extern_cxx=*/false, .extra_args = extra_args});
  EXPECT_NE(r.h_content.find(
                "ALIAS_LIB_EXPORT template <int N>\nusing Foo"),
            std::string::npos)
      << "internal alias template used via dependent-name by another module "
         "must be exported";
}

TEST(FullRewrite, FriendOfExternCxxClassGetsInjectedForwardDecl) {
  // def.h defines Foo (also declared by fwd.h in another module) and has a
  // `friend class Foo;` before the definition. Because Foo is an
  // extern "C++" entity, the module needs an `extern "C++"` forward
  // declaration before the friend, otherwise the friend's declaration attaches
  // to the module and conflicts with the global-module definition.
  auto r = rewrite_header(data_path("friend_lib/def.h"), "friend_lib.def", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"friend_lib::Foo"}, .fwd_declared_fqns = {"friend_lib::Foo"}});
  EXPECT_NE(r.cc_content.find("extern \"C++\" class Foo;"),
            std::string::npos)
      << "a friend declaration of a cross-module extern \"C++\" class needs an "
         "extern \"C++\" forward declaration injected before the header body";
}

TEST(FullRewrite, FriendTargetOfExternCxxClassGetsExternCxx) {
  // core.h defines A (forward-declared by alpha.h, so A is extern "C++") with
  // a `friend class B;` inside it. The friend declaration attaches to the
  // global module, so B's definition in the same module must also be
  // `extern "C++"` or it conflicts with the friend's global-module declaration.
  auto r = rewrite_header(data_path("friend_lib/core.h"), "friend_lib.core", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"friend_lib::A"}, .fwd_declared_fqns = {"friend_lib::A"}});
  EXPECT_NE(r.h_content.find("FRIEND_LIB_EXPORT extern \"C++\" class A {"),
            std::string::npos)
      << "the cross-module class A must be extern \"C++\"";
  EXPECT_NE(r.h_content.find("FRIEND_LIB_EXPORT extern \"C++\" class B {"),
            std::string::npos)
      << "a class friend-declared inside an extern \"C++\" class must itself "
         "be extern \"C++\"";
}

TEST(FullRewrite, FriendDeclaredEntityDefinedByConsumerNotExportedInHeader) {
  // The library forward-declares and friend-declares an internal class that a
  // consumer defines itself. The header rewrite must not export the forward
  // declaration (it would collide with the consumer's global-module
  // definition), but the declaration must be `extern "C++"` so the consumer's
  // definition merges with it and the module's friend declaration keeps
  // granting access to the private members.
  auto producer = data_path("frienddef_lib/producer.h");
  auto consumer = data_path("frienddef_lib/consumer.cc");
  auto reachable = trace_consumer_sources(
      {producer}, {consumer}, "frienddef_lib", {"-I", gDataDir});
  std::vector<std::string> fqns;
  for (auto &[p, list] : reachable)
    for (auto &f : list) fqns.push_back(f);
  auto r = rewrite_header(producer, "frienddef_lib.producer",
                          RewriteOptions{.reachable_fqns = fqns,
                                         .extern_cxx = false,
                                         .cc_only = true});
  EXPECT_EQ(r.cc_content.find("FRIENDDEF_LIB_EXPORT class Gen;"),
            std::string::npos)
      << "a forward declaration of an entity the consumer defines must not be "
         "exported";
  EXPECT_NE(r.cc_content.find("extern \"C++\" class Gen;"), std::string::npos)
      << "the forward declaration must be extern \"C++\" so the consumer's "
         "definition merges with it in the global module";
  EXPECT_EQ(r.cc_content.find("FRIENDDEF_LIB_EXPORT extern \"C++\" class Gen;"),
            std::string::npos)
      << "the extern \"C++\" forward declaration must not also be exported";
}

TEST(FullRewrite, ImplGmfKeepsFeatureTestVersion) {
  // An impl unit whose code is guarded by a library feature-test macro
  // (`#ifdef __cpp_lib_char8_t`) gets the macro only transitively via the C++
  // stdlib headers it includes (<string_view> pulls in <version>). Replacing
  // those headers with `import std.compat` must leave <version> in the GMF, or
  // the guarded definition compiles out and the linker loses the symbol.
  auto r = rewrite_source(data_path("featlib/use.cc"), "featlib.use",
                          {}, {"-I", gDataDir}, /*import_std=*/true);
  EXPECT_NE(r.content.find("#include <version>"), std::string::npos)
      << "the impl GMF must keep <version> so __cpp_lib_* guards evaluate";
  EXPECT_NE(r.content.find("write_u8"), std::string::npos)
      << "the guarded definition must stay in the module body";
}

TEST(FullRewrite, SourceBecomesImplementationUnit) {
  auto r = rewrite_source(data_path("full_lib/impl.cc"), "full_lib.impl");
  auto cc = r.content;
  EXPECT_NE(cc.find("module full_lib.impl;"), std::string::npos)
      << "implementation units use a bare `module X;` declaration";
  EXPECT_EQ(cc.find("export module"), std::string::npos)
      << "implementation units must not be export modules";
  EXPECT_NE(cc.find("import full_lib.a;"), std::string::npos)
      << "library include must become an import";
  EXPECT_EQ(cc.find("#include \"full_lib/a.h\""), std::string::npos)
      << "library include must be removed from the body";
  auto module_pos = cc.find("module full_lib.impl;");
  auto string_pos = cc.find("#include <string>");
  EXPECT_NE(string_pos, std::string::npos);
  EXPECT_LT(string_pos, module_pos)
      << "system includes must move into the global module fragment";
  EXPECT_NE(cc.find("std::string foo"), std::string::npos)
      << "implementation body must be preserved";
}

TEST(FullRewrite, ImplementationUnitImportsArePlain) {
  auto r = rewrite_source(data_path("full_lib/impl.cc"), "full_lib.impl");
  EXPECT_NE(r.content.find("import full_lib.a;"), std::string::npos);
  EXPECT_EQ(r.content.find("export import"), std::string::npos)
      << "implementation units never re-export";
}

TEST(FullRewrite, NoExternCxxByDefault) {
  auto r = rewrite_source(data_path("full_lib/impl.cc"), "full_lib.impl");
  EXPECT_EQ(r.content.find("extern \"C++\""), std::string::npos)
      << "full rewrite mode must not wrap implementation units in extern "
         "\"C++\" by default";
}

TEST(FullRewrite, ExternCxxWhenEnabled) {
  auto r = rewrite_source(data_path("full_lib/impl.cc"), "full_lib.impl",
                          {}, {}, false, /*extern_cxx=*/true);
  EXPECT_NE(r.content.find("extern \"C++\""), std::string::npos)
      << "extern C++ flag must wrap the implementation unit body";
  EXPECT_NE(r.content.find("}  // extern \"C++\""), std::string::npos);
}

// Dual mode: the source stays a plain translation unit and the module
// implementation unit becomes a separate preamble file that includes it.
// `module;` has to be the first token of a translation unit (clang rejects it
// even behind a taken `#ifdef`), so the two modes cannot share one file.
TEST(FullRewrite, DualImplKeepsBodyCompilableAsPlainSource) {
  auto r = rewrite_source(data_path("full_lib/impl.cc"), "full_lib.impl",
                          {}, {}, false, false, {}, {}, {}, {}, false, {},
                          /*use_modules_macro=*/"FULL_LIB_USE_MODULES");
  EXPECT_EQ(r.content.find("module full_lib.impl;"), std::string::npos)
      << "the body file is a plain translation unit, not a module unit";
  EXPECT_EQ(r.content.find("module;"), std::string::npos)
      << "the body file must not open a global module fragment";
  EXPECT_NE(r.content.find("#include \"full_lib/a.h\""), std::string::npos)
      << "the body keeps its includes so it still compiles on its own";
  EXPECT_NE(r.content.find("#include <string>"), std::string::npos);
  EXPECT_NE(r.content.find("#ifndef FULL_LIB_USE_MODULES"), std::string::npos)
      << "the includes are guarded so the module unit can compile them out";
  EXPECT_NE(r.content.find("#endif  // FULL_LIB_USE_MODULES"),
            std::string::npos);
  EXPECT_NE(r.content.find("std::string foo"), std::string::npos)
      << "the body itself is unchanged";
}

TEST(FullRewrite, DualImplGuardsOneIncludeBlockOnce) {
  auto r = rewrite_source(data_path("full_lib/impl.cc"), "full_lib.impl",
                          {}, {}, false, false, {}, {}, {}, {}, false, {},
                          "FULL_LIB_USE_MODULES");
  auto count = [&](std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t p = r.content.find(needle); p != std::string::npos;
         p = r.content.find(needle, p + 1))
      ++n;
    return n;
  };
  EXPECT_EQ(count("#ifndef FULL_LIB_USE_MODULES"), 1u)
      << "adjacent includes must share a single guard";
  EXPECT_EQ(count("#endif  // FULL_LIB_USE_MODULES"), 1u);
}

TEST(FullRewrite, DualImplEmitsModuleUnitThatIncludesTheBody) {
  auto r = rewrite_source(data_path("full_lib/impl.cc"), "full_lib.impl",
                          {}, {}, false, false, {}, {}, {}, {}, false, {},
                          "FULL_LIB_USE_MODULES");
  auto &mc = r.module_content;
  ASSERT_FALSE(mc.empty()) << "dual mode must emit a module unit";
  EXPECT_EQ(mc.find("module;"), 0u)
      << "the global module fragment must open the translation unit";
  EXPECT_NE(mc.find("module full_lib.impl;"), std::string::npos);
  EXPECT_EQ(mc.find("export module"), std::string::npos);
  EXPECT_NE(mc.find("import full_lib.a;"), std::string::npos)
      << "library includes still become imports in the module unit";
  auto define_pos = mc.find("#define FULL_LIB_USE_MODULES 1");
  auto include_pos = mc.find("#include \"../impl.cc\"");
  EXPECT_NE(define_pos, std::string::npos)
      << "the module unit compiles the body's includes out itself";
  EXPECT_NE(include_pos, std::string::npos);
  EXPECT_LT(define_pos, include_pos);
  EXPECT_LT(mc.find("module full_lib.impl;"), define_pos)
      << "the body is included in the module purview";
  EXPECT_LT(mc.find("#include <string>"), mc.find("module full_lib.impl;"))
      << "system includes stay in the global module fragment";
}

TEST(FullRewrite, ImplementationUnitHasNoModuleContentByDefault) {
  auto r = rewrite_source(data_path("full_lib/impl.cc"), "full_lib.impl");
  EXPECT_TRUE(r.module_content.empty())
      << "without dual mode the rewrite stays a single module unit";
}

TEST(FullRewrite, HeaderInterfaceWithoutExternCxx) {
  auto r = rewrite_header(data_path("full_lib/a.h"), "full_lib.a", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false});
  EXPECT_NE(r.cc_content.find("export module full_lib.a;"), std::string::npos);
  EXPECT_EQ(r.cc_content.find("extern \"C++\""), std::string::npos)
      << "header module interface must not be wrapped by default in full mode";
}

TEST(FullRewrite, HeaderInterfaceWithExternCxx) {
  auto r = rewrite_header(data_path("full_lib/a.h"), "full_lib.a", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/true});
  EXPECT_NE(r.cc_content.find("export module full_lib.a;"), std::string::npos);
  EXPECT_NE(r.cc_content.find("extern \"C++\""), std::string::npos);
}

TEST(FullRewrite, RedundantForwardDeclIsRemoved) {
  // a.h forward-declares Foo (defined in b.h, which a.h includes). The
  // defining header becomes an import, so the redundant forward declaration
  // must be deleted from the module.
  auto r = rewrite_header(data_path("fwd_lib/a.h"), "fwd_lib.a", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwd_lib::Foo"}});
  EXPECT_EQ(r.h_content.find("class Foo;"), std::string::npos)
      << "redundant forward declaration of an imported class must be removed";
}

TEST(FullRewrite, CrossModuleForwardDeclGetsExternCxx) {
  // c.h forward-declares Bar (defined in d.h, which c.h does NOT include).
  // A module-private forward declaration would be a distinct type from the
  // defining module's entity, so the declaration must be `extern "C++"` (a
  // global-module entity) to merge with the definition across modules. It must
  // not be exported (the defining module owns the export).
  auto r = rewrite_header(data_path("fwd_lib/c.h"), "fwd_lib.c", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwd_lib::Bar"}});
  EXPECT_NE(r.h_content.find("extern \"C++\" class Bar;"),
            std::string::npos)
      << "cross-module forward declaration must be extern \"C++\" so it "
         "merges with the defining module's entity";
  EXPECT_EQ(r.h_content.find("FWD_LIB_EXPORT class Bar;"),
            std::string::npos)
      << "a forward declaration of a class defined in another module must not "
         "be exported";
  EXPECT_EQ(r.h_content.find("FWD_LIB_EXPORT extern \"C++\" class Bar;"),
            std::string::npos)
      << "the declaration must not be exported even with the extern \"C++\"";
}

TEST(FullRewrite, CrossModuleDefinitionGetsExternCxx) {
  // d.h defines Bar, which another module (c.h) forward-declares. The
  // definition must carry `extern "C++"` so it is the same entity as the
  // forward declaration in the other module.
  auto r = rewrite_header(data_path("fwd_lib/d.h"), "fwd_lib.d", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwd_lib::Bar"}, .fwd_declared_fqns = {"fwd_lib::Bar"}});
  EXPECT_NE(r.h_content.find("FWD_LIB_EXPORT extern \"C++\" class Bar {"),
            std::string::npos)
      << "definition of a cross-module forward-declared class must be "
         "extern \"C++\"";
  EXPECT_EQ(r.h_content.find("extern \"C++\" FWD_LIB_EXPORT"),
            std::string::npos)
      << "the extern \"C++\" must come after the export marker";
}

TEST(FullRewrite, OpaqueForwardDeclStaysExported) {
  // e.h forward-declares Secret, which is never defined anywhere. It must stay
  // exported (not wrapped, not deleted) so consumers can reference it.
  auto r = rewrite_header(data_path("fwd_lib/e.h"), "fwd_lib.e");
  EXPECT_EQ(r.h_content.find("extern \"C++\""), std::string::npos)
      << "opaque (never-defined) forward declarations must not be wrapped";
  EXPECT_NE(r.h_content.find("FWD_LIB_EXPORT class Secret;"),
            std::string::npos)
      << "opaque forward declarations must stay exported";
}

TEST(FullRewrite, SameFileForwardAndDefinitionStayExported) {
  // f.h forward-declares AND defines Flat in the same file. Both must be
  // exported normally (no wrapping, no deletion).
  auto r = rewrite_header(data_path("fwd_lib/f.h"), "fwd_lib.f");
  EXPECT_EQ(r.h_content.find("extern \"C++\""), std::string::npos)
      << "same-file forward+definition pairs must not be wrapped";
  EXPECT_NE(r.h_content.find("FWD_LIB_EXPORT class Flat;"),
            std::string::npos)
      << "the forward declaration must stay exported";
  EXPECT_NE(r.h_content.find("FWD_LIB_EXPORT class Flat {"), std::string::npos)
      << "the definition must stay exported";
}

TEST(FullRewrite, ImplUnitImportsUmbrellaModule) {
  // impl.cc (module umbrella.impl) includes umbrella.h (the umbrella module
  // `umbrella`). compute_auto_imports skips umbrella imports to avoid header
  // cycles, but an implementation unit may import it.
  auto r = rewrite_source(data_path("umbrella/impl.cc"), "umbrella.impl");
  EXPECT_NE(r.content.find("import umbrella;"), std::string::npos)
      << "implementation unit must import the library's umbrella module";
  EXPECT_EQ(r.content.find("#include \"umbrella/umbrella.h\""),
            std::string::npos)
      << "the umbrella header must not remain a raw include";
}

TEST(FullRewrite, ImplUnitKeepsUsedTransitiveSystemInclude) {
  // use.cc includes syslib/defs.h (→ import syslib.defs) and uses std::string,
  // which defs.h includes transitively. The used <string> must be kept in the
  // implementation unit's GMF.
  auto r = rewrite_source(data_path("syslib/use.cc"), "syslib.use",
                          {}, {"-I", gDataDir});
  auto module_pos = r.content.find("module syslib.use;");
  auto string_pos = r.content.find("#include <string>");
  EXPECT_NE(string_pos, std::string::npos)
      << "used transitive system include must stay in the GMF";
  EXPECT_LT(string_pos, module_pos)
      << "transitive system include must be in the GMF before the module "
         "declaration";
  EXPECT_NE(r.content.find("import syslib.defs;"), std::string::npos)
      << "library header must become an import";
}

TEST(FullRewrite, RedundantFunctionForwardDeclIsRemoved) {
  // h.h forward-declares format (defined inline in g.h, which h.h includes).
  // The redundant function forward declaration must be removed.
  auto r = rewrite_header(data_path("fwd_lib/h.h"), "fwd_lib.h", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwd_lib::format"}});
  EXPECT_EQ(r.h_content.find("std::string format(int value);"),
            std::string::npos)
      << "redundant function forward declaration must be removed";
}

TEST(FullRewrite, CrossModuleFunctionForwardDeclGetsExternCxx) {
  // i.h forward-declares render (defined in j.h, which i.h does NOT include).
  // The declaration must stay in the module as `extern "C++"` (a global-module
  // entity) so it merges with j.h's definition, without an export marker.
  auto r = rewrite_header(data_path("fwd_lib/i.h"), "fwd_lib.i", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwd_lib::render"}});
  EXPECT_NE(r.h_content.find("extern \"C++\" std::string render(int value);"),
            std::string::npos)
      << "cross-module function forward declaration must be extern \"C++\"";
  EXPECT_EQ(r.h_content.find("FWD_LIB_EXPORT std::string render(int value);"),
            std::string::npos)
      << "a function forward declaration defined in another module must not be "
         "exported";
}

TEST(FullRewrite, CrossModuleFunctionDefinitionGetsExternCxx) {
  // j.h defines render, which another module (i.h) forward-declares. The
  // definition must carry `extern "C++"` so it is the same entity as the
  // forward declaration in the other module.
  auto r = rewrite_header(data_path("fwd_lib/j.h"), "fwd_lib.j", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwd_lib::render"}, .fwd_declared_fqns = {"fwd_lib::render"}});
  EXPECT_NE(r.h_content.find(
                "FWD_LIB_EXPORT extern \"C++\" std::string render(int value)"),
            std::string::npos)
      << "definition of a cross-module forward-declared function must be "
         "extern \"C++\"";
}

TEST(FullRewrite, InjectedCrossModuleForwardDeclGetsExternCxx) {
  // k.h's code returns Baz*, which is only forward-declared in l.h (an
  // imported module) and defined in m.h (not imported here). The module needs
  // its own forward declaration, and it must be `extern "C++"` so it merges
  // with the defining module's entity.
  auto r = rewrite_header(data_path("fwd_lib/k.h"), "fwd_lib.k", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwd_lib::Baz"}});
  EXPECT_NE(r.cc_content.find(
                "namespace fwd_lib {\nextern \"C++\" class Baz;\n}"),
            std::string::npos)
      << "injected cross-module forward declaration must be extern \"C++\" so "
         "it merges with the defining module's entity";
}

TEST(FullRewrite, InjectedSameModuleForwardDeclGetsExternCxx) {
  // n.h both forward-declares Baz (via included l.h) and defines it in the
  // same file. Because Baz is also declared by another module, the
  // definition here is `extern "C++"`, so the injected forward declaration
  // must be `extern "C++"` too — a plain `export class Baz;` would be a
  // distinct module entity and leave the body's definition unreachable.
  // It must also be exported: this module defines and exports Baz, and
  // [module.interface]/6 only implicitly exports redeclarations of an entity
  // *introduced* by an exported declaration, so the injected declaration
  // preceding the exported definition has to carry the export itself.
  auto r = rewrite_header(data_path("fwd_lib/n.h"), "fwd_lib.n", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"fwd_lib::Baz"}, .fwd_declared_fqns = {"fwd_lib::Baz"}});
  EXPECT_NE(r.cc_content.find(
                "namespace fwd_lib {\nexport extern \"C++\" class Baz;\n}"),
            std::string::npos)
      << "injected forward declaration of a same-module-defined entity that is "
         "also declared by another module must be exported extern \"C++\"";
  EXPECT_EQ(r.cc_content.find("namespace fwd_lib {\nexport class Baz;\n}"),
            std::string::npos)
      << "a plain exported forward declaration would not merge with the "
         "extern \"C++\" definition";
}

TEST(FullRewrite, TemplateForwardDeclGetsExternCxx) {
  // a.h forward-declares the class template Foo (defined in b.h, which a.h
  // does NOT include). The `extern "C++"` must be placed before the
  // `template <...>` clause; inserting it after the template parameter list is
  // ill-formed.
  auto r = rewrite_header(data_path("tpl_lib/a.h"), "tpl_lib.a", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"tpl_lib::Foo"}});
  EXPECT_NE(r.h_content.find("extern \"C++\" template <typename T>"),
            std::string::npos)
      << "extern \"C++\" must precede the template clause";
  EXPECT_EQ(r.h_content.find("template <typename T>\nextern \"C++\""),
            std::string::npos)
      << "extern \"C++\" must not be inserted after the template parameter "
         "list";
}

TEST(FullRewrite, TemplateDefinitionGetsExternCxx) {
  // b.h defines the class template Foo (forward-declared by a.h in another
  // module). The definition AND its explicit specialization must carry
  // `extern "C++"` before the template clause so they merge with the other
  // module's declaration.
  auto r = rewrite_header(data_path("tpl_lib/b.h"), "tpl_lib.b", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"tpl_lib::Foo"}, .fwd_declared_fqns = {"tpl_lib::Foo"}});
  EXPECT_NE(r.h_content.find("TPL_LIB_EXPORT extern \"C++\" template <typename T>"),
            std::string::npos)
      << "definition of a cross-module forward-declared class template must be "
         "extern \"C++\"";
  EXPECT_NE(r.h_content.find("extern \"C++\" template <>\nstruct Foo<int>"),
            std::string::npos)
      << "explicit specialization of a cross-module forward-declared class "
         "template must also be extern \"C++\"";
  EXPECT_EQ(r.h_content.find("TPL_LIB_EXPORT extern \"C++\" template <>"),
            std::string::npos)
      << "[module.interface]/3: the explicit specialization must not be "
         "exported; it is exported with its primary template";
}

TEST(FullRewrite, ImplUnitExternCxxMemberDefinition) {
  // thing.cc defines Foo::size(), and Foo is forward-declared by another
  // header (fwd.h), so its definition in the interface is `extern "C++"`. The
  // member definition in the implementation unit must also be `extern "C++"`
  // to attach to the same global-module entity.
  auto r = rewrite_source(data_path("impl_lib/thing.cc"), "impl_lib.thing",
                          {}, {"-I", gDataDir}, false, false, {}, {},
                          {}, {"impl_lib::Foo"});
  EXPECT_NE(r.content.find("extern \"C++\" int Foo::size() const"),
            std::string::npos)
      << "member definition of a cross-module forward-declared class must be "
         "extern \"C++\" in the implementation unit";
  EXPECT_EQ(r.content.find("export module"), std::string::npos)
      << "implementation units use a bare `module X;` declaration";
}

TEST(FullRewrite, ImplUnitNoExternCxxForPlainMember) {
  // A member definition of a class that is NOT cross-module forward-declared
  // must stay a plain module definition.
  auto r = rewrite_source(data_path("impl_lib/thing.cc"), "impl_lib.thing",
                          {}, {"-I", gDataDir});
  EXPECT_EQ(r.content.find("extern \"C++\" int Foo::size() const"),
            std::string::npos)
      << "no extern \"C++\" without a cross-module forward-declared class";
}

TEST(FullRewrite, OutOfLineMemberClassIsDetected) {
  // thing.cc defines Foo::size() out-of-line. The impl unit lives in its own
  // module, so Foo (declared in api.h) must be extern "C++" for the member
  // definition to be valid across modules.
  auto fqns = out_of_line_member_class_fqns(
      {data_path("impl_lib/thing.cc")}, {"-I", gDataDir});
  EXPECT_NE(std::find(fqns.begin(), fqns.end(), "impl_lib::Foo"),
            fqns.end())
      << "a class whose member is defined out-of-line in an implementation "
         "file must be detected";
}

TEST(FullRewrite, MemberTemplateDefinitionExternCxxBeforeOuterHeader) {
  // c.h defines `Box<T>::Box(U)` — a member template whose out-of-line
  // definition has TWO template headers (`template <typename T>` from the
  // class and `template <typename U>` from the member). When the class is
  // extern "C++" the marker must go before the OUTERMOST template header, not
  // between the two headers (which is ill-formed C++).
  auto r = rewrite_header(data_path("memtpl_lib/c.h"), "memtpl_lib.c", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"memtpl_lib::Box"}, .fwd_declared_fqns = {"memtpl_lib::Box"}});
  EXPECT_NE(r.h_content.find(
                "extern \"C++\" template <typename T>\ntemplate <typename U>\n"
                "Box<T>::Box(U)"),
            std::string::npos)
      << "the extern \"C++\" marker must precede both template headers";
  EXPECT_EQ(
      r.h_content.find(
          "template <typename T>\nextern \"C++\" template <typename U>"),
      std::string::npos)
      << "the marker must not land between the two template headers";
}

TEST(FullRewrite, OutOfLineMemberInHeaderGetsExternCxx) {
  // c.h defines Baz<T>::Baz() out-of-line inside the header. When the class is
  // extern "C++" (a global-module entity), the out-of-line member definition
  // must also be `extern "C++"`, or it would be a distinct module entity that
  // conflicts with the class's global-module member declaration.
  auto r = rewrite_header(data_path("tpl_lib/c.h"), "tpl_lib.c", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"tpl_lib::Baz"}, .fwd_declared_fqns = {"tpl_lib::Baz"}});
  EXPECT_NE(r.h_content.find(
                "extern \"C++\" template <typename T>\nBaz<T>::Baz()"),
            std::string::npos)
      << "an out-of-line member definition of an extern \"C++\" class template "
         "must also be extern \"C++\"";
}

TEST(FullRewrite, StaticFunctionNotExported) {
  // Namespace-scope `static` functions have internal linkage and must never
  // get the export marker.
  auto r = rewrite_header(data_path("stat_lib/api.h"), "stat_lib.api");
  EXPECT_EQ(r.h_content.find(
                "STAT_LIB_EXPORT template <typename T>\nstatic void Delete"),
            std::string::npos)
      << "a static (internal-linkage) function must not be exported";
  EXPECT_NE(r.h_content.find("static void Delete"), std::string::npos)
      << "the static function must remain";
}

TEST(FullRewrite, OutOfLineStaticMemberVarClassDetected) {
  // staticvar.cc defines Baz::count (a static member variable) and
  // Baz::size() out-of-line. The class must be detected for extern "C++".
  auto fqns = out_of_line_member_class_fqns(
      {data_path("impl_lib/staticvar.cc")}, {"-I", gDataDir});
  EXPECT_NE(std::find(fqns.begin(), fqns.end(), "impl_lib::Baz"),
            fqns.end())
      << "a class with out-of-line static member-variable definitions must be "
         "detected";
}

TEST(FullRewrite, OutOfLineMemberClassSkipsImplLocalClass) {
  // local.cc defines class Box entirely within the implementation file. It is
  // local to the impl module, so it must NOT be detected for extern "C++".
  auto fqns = out_of_line_member_class_fqns(
      {data_path("impl_lib/local.cc")}, {"-I", gDataDir});
  EXPECT_EQ(std::find(fqns.begin(), fqns.end(), "impl_lib::Box"),
            fqns.end())
      << "a class defined entirely in the implementation file is module-local "
         "and must not be extern \"C++\"";
}

TEST(FullRewrite, ImplUnitSkipsInClassMemberDefinition) {
  // inline.cc has an inline ctor/method definition inside the class. These
  // must not get `extern "C++"` even when the class is extern "C++".
  auto r = rewrite_source(data_path("impl_lib/inline.cc"), "impl_lib.inline",
                          {}, {"-I", gDataDir}, false, false, {}, {},
                          {}, {"impl_lib::Gen"});
  EXPECT_EQ(r.content.find("extern \"C++\" explicit Gen("),
            std::string::npos)
      << "an in-class member definition must not be prefixed with extern "
         "\"C++\"";
  EXPECT_EQ(r.content.find("extern \"C++\" int get()"),
            std::string::npos)
      << "an in-class member definition must not be prefixed with extern "
         "\"C++\"";
}

TEST(FullRewrite, ImplKeepsUsedTransitiveSystemInclude) {
  // use.cc reaches <regex.h> transitively through api.h and calls regcomp
  // directly. The impl GMF must keep the used system header.
  auto r = rewrite_source(data_path("sysinc_lib/use.cc"), "sysinc_lib.use",
                          {}, {"-I", gDataDir});
  EXPECT_NE(r.content.find("#include <regex.h>"), std::string::npos)
      << "a system header reached through a library header and used by the "
         "impl body must stay in the GMF";
}

TEST(FullRewrite, ImplStaticFunctionGetsNoExternCxx) {
  // A static (internal-linkage) free function in an impl unit must never be
  // prefixed with extern "C++", even when its name is in fwd_declared_fqns
  // (e.g. it shares a name with a cross-module overload).
  auto r = rewrite_source(data_path("impl_lib/staticfn.cc"), "impl_lib.staticfn",
                          {}, {"-I", gDataDir}, false, false, {}, {}, {},
                          {"impl_lib::foo"});
  EXPECT_EQ(r.content.find("extern \"C++\" static int foo"),
            std::string::npos)
      << "a static (internal-linkage) function must not get extern \"C++\"";
}

TEST(FullRewrite, NestedClassMemberInImplGetsExternCxx) {
  // Foo::Bar::connect() is an out-of-line member definition of a nested
  // class whose enclosing class (Foo) is cross-module extern "C++". The
  // definition must also be extern "C++".
  auto r = rewrite_source(data_path("nested_lib/impl.cc"), "nested_lib.impl",
                          {}, {"-I", gDataDir}, false, false, {}, {}, {},
                          {"nested_lib::Foo"});
  EXPECT_NE(r.content.find("extern \"C++\" void Foo::Bar::connect()"),
            std::string::npos)
      << "a method of a nested class inside an extern \"C++\" class must be "
         "extern \"C++\"";
}

TEST(FullRewrite, FreeClassDefinedInImplIsDetected) {
  // impl.cc defines `class Bar`, which core.h friend-declares inside
  // `class Foo`. The class has no declaration in any library header, so its
  // definition is invisible when the header is analyzed alone. It must be
  // `extern "C++"` on both the friend declaration and the impl definition to
  // keep the friendship valid across modules (TestResult/ExecDeathTest is the
  // real-world case).
  auto fqns = out_of_line_defined_free_classes(
      {data_path("friendimpl_lib/impl.cc")}, {"-I", gDataDir});
  EXPECT_NE(std::find(fqns.begin(), fqns.end(), "friendimpl_lib::Bar"),
            fqns.end())
      << "a class defined only in a source file and friend-declared in a "
         "header must be detected for extern \"C++\"";
}

TEST(FullRewrite, FriendExternTargetDetected) {
  // core.h declares class Foo with `friend class Bar`. When Foo is an
  // extern "C++" class, Bar attaches to the global module and must be
  // extern "C++" too. The analyzer must surface Bar.
  auto fqns = friend_extern_fqns(
      {data_path("friendimpl_lib/core.h")}, {"-I", gDataDir},
      {"friendimpl_lib::Foo"});
  EXPECT_NE(std::find(fqns.begin(), fqns.end(), "friendimpl_lib::Bar"),
            fqns.end())
      << "a friend target of an extern \"C++\" class must be detected";
}

TEST(FullRewrite, FriendTargetImplGetsExternCxx) {
  // impl.cc defines Bar, friend-declared inside extern "C++" class Foo.
  // The impl definition must be extern "C++" to merge with the friend's
  // global-module declaration.
  auto r = rewrite_source(data_path("friendimpl_lib/impl.cc"),
                          "friendimpl_lib.impl", {}, {"-I", gDataDir}, false,
                          false, {}, {}, {},
                          {"friendimpl_lib::Bar"});
  EXPECT_NE(r.content.find("extern \"C++\" class Bar"), std::string::npos)
      << "a friend target defined in an impl file must be extern \"C++\"";
}

TEST(FullRewrite, ExternCxxClassBaseChain) {
  // decl.h forward-declares Bar; impl.cc defines Foo and Bar (which
  // derives from Foo). A global-module (extern "C++") class cannot derive from
  // a module-entity class, so the whole base chain must be extern "C++".
  auto r = rewrite_source(data_path("chain_lib/impl.cc"), "chain_lib.impl",
                          {}, {"-I", gDataDir}, false, false, {}, {}, {},
                          {"chain_lib::Bar"});
  EXPECT_NE(r.content.find("extern \"C++\" class Foo"), std::string::npos)
      << "the base of an extern \"C++\" derived class must also be "
         "extern \"C++\"";
  EXPECT_NE(r.content.find("extern \"C++\" class Bar"), std::string::npos)
      << "the cross-module derived class must be extern \"C++\"";
}

TEST(FullRewrite, DefaultValueGuardMacroIsExtracted) {
  // DEFAULT_LIB_DEFAULT_STYLE is a default-value macro guarded by a previous
  // `#ifndef` line. It must still be extracted into the macros file so impl
  // units (which include that file in their GMF) can use it.
  auto r = rewrite_header(data_path("default_lib/api.h"), "default_lib.api");
  EXPECT_NE(r.macros_content.find(
                "define DEFAULT_LIB_DEFAULT_STYLE"),
            std::string::npos)
      << "a default-value macro guarded by `#ifndef` must be extracted";
}

TEST(FullRewrite, ReachableStaticHelperIsExported) {
  // foo is a static helper in a header that consumers (impl units)
  // use. It is defined only in this header (no forward declaration in another
  // module), so it just needs `static` removed and a plain export — no extern
  // "C++" — for impl units importing the module to use it.
  auto r = rewrite_header(data_path("helpremit_lib/api.h"), "helpremit_lib.api", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {"helpremit_lib::foo"}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {}});
  EXPECT_NE(r.h_content.find(
                "HELPREMIT_LIB_EXPORT void foo()"),
            std::string::npos)
      << "a consumer-reachable static helper must be exported (static "
         "removed)";
  EXPECT_EQ(r.h_content.find("static void foo"), std::string::npos)
      << "the static keyword must be removed";
  EXPECT_EQ(r.h_content.find("HELPREMIT_LIB_EXPORT extern \"C++\" void "
                             "foo"),
            std::string::npos)
      << "a helper with no cross-module forward declaration must not be "
         "extern \"C++\"";
}

TEST(FullRewrite, ImplImportsUsedTransitiveSubModule) {
  // use.cc includes root.h but references sub_value() from sub.h (reached
  // transitively). The impl must import the module that defines it, since the
  // umbrella does not re-export internal sub-modules.
  auto r = rewrite_source(data_path("translib/use.cc"), "translib.use",
                          {}, {"-I", gDataDir});
  EXPECT_NE(r.content.find("import translib.sub;"), std::string::npos)
      << "an impl unit must import the module of a transitively-included "
         "header it uses";
}

TEST(FullRewrite, ImplIncludesUmbrellaMacrosUnconditionally) {
  // use.cc uses UMBRELLA_LIB_FLAG, a macro from the umbrella umbrella_lib.h,
  // without importing the umbrella module. The impl GMF must include the
  // umbrella macros file so the macro is available.
  auto r = rewrite_source(data_path("umbrella_lib/use.cc"), "umbrella_lib.use",
                          {}, {"-I", gDataDir}, false, false, {},
                          {"umbrella_lib/umbrella_lib_macros.h"});
  EXPECT_NE(r.content.find("#include \"umbrella_lib/umbrella_lib_macros.h\""),
            std::string::npos)
      << "an impl unit must include the umbrella macros file";
}

TEST(FullRewrite, ReachableStaticTemplateIsExported) {
  // Delete is a static function template in an internal namespace that
  // consumers use (e.g. impl.cc calls internal::Delete<TestInfo>). It is only
  // defined in this header (no cross-module forward declaration), so `static`
  // is stripped and it is exported as a plain template — no extern "C++" —
  // for impl units to use.
  auto r = rewrite_header(data_path("statreach_lib/api.h"), "statreach_lib.api", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {"statreach_lib::Delete"}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {}});
  EXPECT_NE(r.h_content.find("STATREACH_LIB_EXPORT template "
                             "<typename T>\nvoid Delete"),
            std::string::npos)
      << "a consumer-reachable static function template must be exported "
         "(static removed)";
  EXPECT_EQ(r.h_content.find("static void Delete"), std::string::npos)
      << "the static keyword must be removed";
}

TEST(FullRewrite, InternalCrossModuleClassIsExported) {
  // Foo lives in an internal namespace but its members are defined in an
  // impl unit (a different module), so it is extern "C++" AND must be exported
  // for the impl unit to see its declaration.
  auto r = rewrite_header(data_path("internexport_lib/api.h"), "internexport_lib.api", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {}, .fwd_declared_fqns = {"internexport_lib::internal::Foo"}});
  EXPECT_NE(r.h_content.find(
                "INTERNEXPORT_LIB_EXPORT extern \"C++\" class Foo"),
            std::string::npos)
      << "an internal-namespace class whose members are defined in an impl "
         "unit must be exported as well as extern \"C++\"";
}

TEST(FullRewrite, ImplDefinedClassGetsExternCxx) {
  // api3.h forward-declares Bar; widget.cc defines it. The defining module
  // differs from the declaring one, so the impl definition must be extern
  // "C++" to merge with the header's global-module declaration.
  auto r = rewrite_source(data_path("impl_lib/widget.cc"), "impl_lib.widget",
                          {}, {"-I", gDataDir}, false, false, {}, {},
                          {}, {"impl_lib::Bar"});
  EXPECT_NE(r.content.find("extern \"C++\" class Bar"), std::string::npos)
      << "a class defined in an impl file but declared in a library header "
         "must be extern \"C++\" in the impl";
  EXPECT_NE(r.content.find("int foo()"),
            std::string::npos)
      << "the free function remains";
}

TEST(FullRewrite, SameModuleImplFreeEntityNotExternCxx) {
  // api.h declares `int flag;` and api.cc (the same-stem source) defines it.
  // In run_full_rewrite the source becomes an impl unit of the SAME module as
  // the header (stem-to-module match), so neither side needs `extern "C++"`.
  // out_of_line_defined_free_fqns must not flag the entity for extern "C++"
  // when the defining source is a same-module impl unit.
  auto fqns = out_of_line_defined_free_fqns(
      {data_path("sameimpl_lib/api.cc")}, {"-I", gDataDir});
  EXPECT_EQ(std::find(fqns.begin(), fqns.end(), "sameimpl_lib::flag"),
            fqns.end())
      << "a free variable defined in a same-module impl source must not be "
         "flagged for extern \"C++\"";
}

TEST(FullRewrite, OutOfLineFreeFunctionInImplIsDetected) {
  // impl.cc defines render(), whose declaration lives in decl.h. The impl
  // module differs from decl.h's module, so render must be extern "C++" on
  // both sides. The analyzer must detect it.
  auto fqns = out_of_line_defined_free_fqns(
      {data_path("crossimpl/impl.cc")}, {"-I", gDataDir});
  EXPECT_NE(std::find(fqns.begin(), fqns.end(), "crossimpl::render"),
            fqns.end())
      << "a free function defined in an impl file but declared in a library "
         "header must be detected for extern \"C++\"";
}

TEST(FullRewrite, CrossModuleFunctionInImplGetsExternCxx) {
  // decl.h declares render(); impl.cc defines it. With render in
  // fwd_declared_fqns, both the header declaration and the impl definition
  // must be `extern "C++"` so they attach to the same global-module entity.
  auto r = rewrite_source(data_path("crossimpl/impl.cc"), "crossimpl.impl",
                          {}, {"-I", gDataDir}, false, false, {}, {},
                          {}, {"crossimpl::render"});
  EXPECT_NE(r.content.find("extern \"C++\" int render("), std::string::npos)
      << "the impl definition of a cross-module free function must be "
         "extern \"C++\"";
  auto h = rewrite_header(data_path("crossimpl/decl.h"), "crossimpl.decl", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"crossimpl::render"}, .fwd_declared_fqns = {"crossimpl::render"}});
  EXPECT_NE(h.h_content.find("extern \"C++\" int render("),
            std::string::npos)
      << "the header declaration of a cross-module free function must also be "
         "extern \"C++\"";
}

TEST(FullRewrite, CrossModuleVariableInImplGetsExternCxx) {
  // vars.h declares g_render_count; vars.cc defines it. Both sides must be
  // `extern "C++"` so they attach to the same global-module entity.
  auto r = rewrite_source(data_path("crossimpl/vars.cc"), "crossimpl.vars",
                          {}, {"-I", gDataDir}, false, false, {}, {},
                          {}, {"crossimpl::g_render_count"});
  EXPECT_NE(r.content.find("extern \"C++\" int g_render_count"),
            std::string::npos)
      << "the impl definition of a cross-module variable must be "
         "extern \"C++\"";
  auto h = rewrite_header(data_path("crossimpl/vars.h"), "crossimpl.vars", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"crossimpl::g_render_count"}, .fwd_declared_fqns = {"crossimpl::g_render_count"}});
  EXPECT_NE(h.h_content.find("CROSSIMPL_EXPORT extern \"C++\" int g_render_count"),
            std::string::npos)
      << "the header declaration of a cross-module variable must also be "
          "extern \"C++\"";
}

TEST(FullRewrite, ImplExternVariableDropsRedundantExtern) {
  // api.h declares `extern int flag;` and api.cc defines it in the SAME
  // module (same stem). The impl rewrite must not wrap the definition in
  // `extern "C++"` (it belongs to the interface's module).
  auto r = rewrite_source(data_path("sameimpl_lib/api.cc"), "sameimpl_lib.api",
                          {}, {"-I", gDataDir}, false, false, {}, {}, {});
  EXPECT_NE(r.content.find("int flag = 42"), std::string::npos)
      << "a same-module impl variable definition must be kept as-is";
  EXPECT_EQ(r.content.find("extern \"C++\" int flag"), std::string::npos)
      << "a same-module impl variable must not be extern \"C++\"";
}

TEST(FullRewrite, ImplMainGetsExternCxx) {
  // A loose implementation file (no same-stem header) with `int main()` is in
  // its own module; a free function defined there and declared across modules
  // must be extern "C++".
  auto r = rewrite_source(data_path("mainlib/main.cc"), "mainlib.main",
                          {}, {"-I", gDataDir}, false, false, {}, {}, {},
                          {"mainlib::main"});
  EXPECT_NE(r.content.find("extern \"C++\""), std::string::npos)
      << "a free function in a loose impl unit must be extern \"C++\"";
  EXPECT_NE(r.content.find("int main"), std::string::npos)
      << "the main definition must be preserved";
}

TEST(FullRewrite, ImplUnitWithOperatorsDoesNotCrash) {
  // A loose impl unit defining operator==/!= must not crash the rewriter and
  // must keep the operator definitions.
  auto r = rewrite_source(data_path("opslib/ops.cc"), "opslib.ops",
                          {}, {"-I", gDataDir}, false, false, {}, {}, {});
  EXPECT_NE(r.content.find("operator=="), std::string::npos)
      << "the operator definition must be preserved";
  EXPECT_NE(r.content.find("operator!="), std::string::npos)
      << "the operator definition must be preserved";
}

TEST(FullRewrite, ImplVarDefinitionWithoutInitStaysDefinition) {
  // counter.h declares `extern int g_counter;`; counter.cc defines
  // `int g_counter;` (no initializer). With g_counter in fwd_declared_fqns the
  // impl definition must be `extern "C++"` BUT remain a definition: prefixing
  // `extern "C++"` alone yields `extern "C++" int g_counter;` which is a mere
  // declaration (extern storage class), leaving the symbol undefined.
  auto r = rewrite_source(data_path("varimpl_lib/counter.cc"),
                          "varimpl_lib.counter", {}, {"-I", gDataDir},
                          false, false, {}, {}, {},
                          {"varimpl_lib::g_counter"});
  EXPECT_NE(r.content.find("extern \"C++\" int g_counter{}"),
            std::string::npos)
      << "a cross-module impl variable definition without an initializer must "
         "gain an empty initializer so it stays a definition";
  EXPECT_EQ(r.content.find("extern \"C++\" int g_counter;"),
            std::string::npos)
      << "a bare `extern \"C++\" int g_counter;` would be a declaration, not a "
         "definition";
}

TEST(FullRewrite, ImplVarDefinitionWithInitKeepsInitializer) {
  // timer.cc defines `long g_elapsed;` with no initializer. A variable that
  // already has an initializer in source must keep it (never be turned into a
  // bare declaration).
  auto r = rewrite_source(data_path("varimpl_lib/timer.cc"),
                          "varimpl_lib.timer", {}, {"-I", gDataDir},
                          false, false, {}, {}, {},
                          {"varimpl_lib::g_elapsed"});
  EXPECT_NE(r.content.find("extern \"C++\" long g_elapsed{}"),
            std::string::npos)
      << "cross-module impl variable without an initializer must stay a "
         "definition";
}

TEST(FullRewrite, OutOfLineStaticMemberVarClassGetsExternCxx) {
  // staticvar.cc defines Baz::count (a static member variable) out-of-line.
  // Baz is declared in api2.h (a different module), so its header
  // definition must be `extern "C++"`.
  auto h = rewrite_header(data_path("impl_lib/api2.h"), "impl_lib.api2", RewriteOptions{.combined_macros = false, .include_to_module = {}, .reachable_fqns = {}, .extern_cxx = /*extern_cxx=*/false, .extra_args = {"-I", gDataDir}, .no_internal_filter = false, .import_std = false, .macro_modules = {}, .internal_mode = InternalMode::kBoth, .module_replaces = {}, .defined_fqns = {"impl_lib::Baz"}, .fwd_declared_fqns = {"impl_lib::Baz"}});
  EXPECT_NE(h.h_content.find("IMPL_LIB_EXPORT extern \"C++\" class Baz"),
            std::string::npos)
      << "a class whose static member variable is defined in an impl module "
         "must be extern \"C++\" in its header";
}

TEST(FullRewrite, ImplUnitIncludesUmbrellaMacros) {
  // leaf.cc uses ROOTLIB_FLAG, a macro defined in the umbrella rootlib.h. The
  // impl unit must include the umbrella macros file so the macro is available.
  auto r = rewrite_source(data_path("rootlib/leaf.cc"), "rootlib.leaf",
                          {}, {"-I", gDataDir}, false, false, {},
                          {"rootlib/rootlib_macros.h"});
  EXPECT_NE(r.content.find("#include \"rootlib/rootlib_macros.h\""),
            std::string::npos)
      << "an impl unit that uses macros from the umbrella header must include "
         "the umbrella macros file";
}

TEST(FullRewrite, FriendPairsFromTheScanMatchTheDedicatedSweep) {
  // Which friend targets matter depends on the set of extern "C++" entities,
  // which is only complete after the template-body scan and the source scans
  // have run — but the friend declarations themselves do not depend on it. So
  // they are collected by a sweep that happens anyway and filtered here, and
  // that must give exactly what the dedicated sweep gave.
  std::vector<std::string> headers = {data_path("friendimpl_lib/core.h")};
  std::vector<std::string> extra_args = {"-I", gDataDir};
  std::vector<std::string> fwd = {"friendimpl_lib::Foo"};

  auto swept = friend_extern_fqns(headers, extra_args, fwd);
  auto scan = cross_module_template_body_referenced_fqns(headers, extra_args);
  auto filtered = friend_extern_fqns_from_pairs(scan.friend_pairs, fwd);

  EXPECT_EQ(filtered, swept)
      << "filtering the collected pairs must equal the dedicated sweep";
  EXPECT_NE(std::find(filtered.begin(), filtered.end(), "friendimpl_lib::Bar"),
            filtered.end())
      << "and must still find the friend target";

  // A class that is not extern "C++" must contribute nothing.
  EXPECT_TRUE(friend_extern_fqns_from_pairs(scan.friend_pairs, {}).empty())
      << "no extern \"C++\" enclosing class means no friend targets";
}

TEST(FullRewrite, TemplateBodyScanRidingAnotherParseGivesTheSameResult) {
  // The template-body scan needs only what the header sweep already produced,
  // so it can run as a rider on the reachability trace's parse instead of
  // sweeping the headers again. Folding the raw per-file collection must give
  // exactly what the standalone sweep gives.
  std::vector<std::string> headers = {data_path("tplbase_lib/defs.h"),
                                      data_path("tplbase_lib/use.h")};
  std::vector<std::string> extra_args = {"-I", gDataDir};

  auto standalone =
      cross_module_template_body_referenced_fqns(headers, extra_args);

  // Drive the same collectors by hand, as the rider does.
  auto models = analyze_files_per_file(headers, extra_args);
  std::map<std::string, std::set<std::string>> defined_files;
  std::set<std::string> alias_fqns;
  for (std::size_t i = 0; i < headers.size(); ++i)
    for (const auto &item : models[i].items) {
      if (!item.complete) continue;
      auto fqn = fqn_of(item.ns_path, item.name);
      defined_files[fqn].insert(headers[i]);
      if (item.kind == EntityItem::kAlias) alias_fqns.insert(fqn);
    }
  TemplateBodyRawScan raw(headers.size());
  for (std::size_t i = 0; i < headers.size(); ++i) {
    std::vector<std::string> one = {headers[i]};
    parallel_parse(one, extra_args, /*delayed_template_parsing=*/false,
                   [&](std::size_t, const std::string &path) {
                     return std::make_unique<VisitorFrontendActionFactory>(
                         [&, self = path](clang::CompilerInstance &ci) {
                           return make_template_body_scan_consumer(
                               ci, defined_files, alias_fqns, self,
                               raw.outs[i], raw.aliases[i],
                               raw.friend_pairs[i]);
                         });
                   });
  }
  auto folded = finalize_template_body_scan(raw);

  EXPECT_EQ(folded.fwd_declared, standalone.fwd_declared);
  EXPECT_EQ(folded.aliases, standalone.aliases);
  EXPECT_EQ(folded.friend_pairs.size(), standalone.friend_pairs.size());
}

// Standard-conformance rules the export marker must respect. clang accepts the
// violations below; they are ill-formed C++ and other implementations reject
// them.
TEST(FullRewrite, NoExportOnTemplateSpecializations) {
  auto r = rewrite_header(data_path("spec_lib/a.h"), "spec_lib.a",
                          RewriteOptions{.no_internal_filter = true});
  auto &cc = r.h_content;
  EXPECT_NE(cc.find("SPEC_LIB_EXPORT template <typename T>\nstruct Holder {"),
            std::string::npos)
      << "the primary template is exported";
  EXPECT_EQ(cc.find("SPEC_LIB_EXPORT template <typename T>\nstruct Holder<T*>"),
            std::string::npos)
      << "[module.interface]/3: no export on a partial specialization";
  EXPECT_EQ(cc.find("SPEC_LIB_EXPORT template <>"), std::string::npos)
      << "[module.interface]/3: no export on an explicit specialization";
  EXPECT_NE(cc.find("struct Holder<T*>"), std::string::npos)
      << "the specialization itself is still emitted";
  EXPECT_NE(cc.find("struct Holder<bool>"), std::string::npos);
}

TEST(FullRewrite, ExportPrecedesLinkageSpecification) {
  auto r = rewrite_header(data_path("spec_lib/a.h"), "spec_lib.a",
                          RewriteOptions{.no_internal_filter = true});
  auto &cc = r.h_content;
  EXPECT_NE(cc.find("SPEC_LIB_EXPORT extern \"C\" int spec_lib_answer"),
            std::string::npos)
      << "the export marker must precede the linkage-specification";
  EXPECT_EQ(cc.find("extern \"C\" SPEC_LIB_EXPORT"), std::string::npos)
      << "`extern \"C\" export` is ill-formed";
}

TEST(FullRewrite, DualBodyImportsWhenCompiledOutsideTheModuleUnit) {
  // The dual body is compiled two ways. Included by the module unit it is
  // attached to the module and needs no imports of its own. Compiled directly
  // with modules on it is an ordinary translation unit — which is the only
  // shape available where implementations cannot attach to a module at all —
  // and an ordinary translation unit sees what it defines only by importing
  // it. Its own module has to come first: `module X;` implied that import.
  auto r = rewrite_source(data_path("full_lib/impl.cc"), "full_lib.impl",
                          {}, {}, false, false, {}, {}, {}, {}, false, {},
                          "FULL_LIB_USE_MODULES");
  auto &body = r.content;

  auto guard = body.find(
      "#if defined(FULL_LIB_USE_MODULES) && !defined(FULL_LIB_MODULE_UNIT)");
  ASSERT_NE(guard, std::string::npos)
      << "the imports must be skipped when the module unit includes the body";
  auto own = body.find("import full_lib.impl;", guard);
  ASSERT_NE(own, std::string::npos)
      << "the body must import the module whose entities it defines";
  auto dep = body.find("import full_lib.a;", guard);
  EXPECT_NE(dep, std::string::npos) << "and everything it used to include";
  EXPECT_LT(own, dep) << "its own module first";

  // The module unit defines the marker before pulling the body in, so that
  // same block is inert there — it must not import its own module.
  auto &mc = r.module_content;
  auto marker = mc.find("#define FULL_LIB_MODULE_UNIT 1");
  ASSERT_NE(marker, std::string::npos)
      << "the module unit must mark the body as attached";
  auto include = mc.find("#include \"../impl.cc\"");
  ASSERT_NE(include, std::string::npos);
  EXPECT_LT(marker, include) << "marked before the body is included";
}
