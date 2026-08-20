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
// ConsumerRewrite suite
TEST(ConsumerRewrite, ImportReplacesLibraryInclude) {
  // A consumer's `#include "lib/header.h"` becomes a module import plus the
  // generated macro-header include (macros do not cross module boundaries).
  ConsumerRewriteOptions cfg;
  cfg.include_to_module = {
      {"lib/lib.h", {"lib", "lib_macros.h"}},
      {"lib/internal/lib-port.h",
       {"lib.internal.lib_port", "lib-port_macros.h"}},
  };
  std::string src =
      "#include <string>\n"
      "#include \"lib/internal/lib-port.h\"\n"
      "#include \"lib/lib.h\"\n"
      "\n"
      "int main() { return 0; }\n";
  auto out = rewrite_consumer_source(src, cfg);
  EXPECT_NE(out.find("import lib;"), std::string::npos)
      << "the umbrella header must become an import of the library module";
  EXPECT_NE(out.find("import lib.internal.lib_port;"), std::string::npos)
      << "a sub-header must become an import of its own module";
  EXPECT_EQ(out.find("#include \"lib/lib.h\""), std::string::npos)
      << "the library include must be removed";
  EXPECT_EQ(out.find("#include \"lib/internal/lib-port.h\""),
            std::string::npos)
      << "the library include must be removed";
  EXPECT_NE(out.find("#include \"lib_macros.h\""), std::string::npos)
      << "the umbrella macro header must be included";
  EXPECT_NE(out.find("#include \"lib-port_macros.h\""), std::string::npos)
      << "the per-header macro file must be included";
}

TEST(ConsumerRewrite, ImportStdReplacesStdIncludes) {
  // With --import-std the consumer's stdlib includes are dropped in favor of
  // a single `import std;`.
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.include_to_module = {
      {"lib/lib.h", {"lib", "lib_macros.h"}},
  };
  std::string src =
      "#include <string>\n"
      "#include <vector>\n"
      "#include \"lib/lib.h\"\n"
      "\n"
      "int main() { return 0; }\n";
  auto out = rewrite_consumer_source(src, cfg);
  EXPECT_NE(out.find("import std.compat;"), std::string::npos)
      << "std.compat also provides the C library's global names a consumer may "
         "rely on";
  EXPECT_EQ(out.find("#include <string>"), std::string::npos)
      << "a stdlib include covered by import std must be dropped";
  EXPECT_EQ(out.find("#include <vector>"), std::string::npos)
      << "a stdlib include covered by import std must be dropped";
  EXPECT_NE(out.find("import lib;"), std::string::npos);
}

TEST(ConsumerRewrite, DefaultEmitsStdIncludeBlock) {
  // Without --import-std the consumer gets an explicit block of the standard
  // headers it historically relied on from the library headers.
  ConsumerRewriteOptions cfg;
  cfg.include_to_module = {
      {"lib/lib.h", {"lib", "lib_macros.h"}},
  };
  std::string src = "#include \"lib/lib.h\"\nint main() {}\n";
  auto out = rewrite_consumer_source(src, cfg);
  EXPECT_NE(out.find("#include <string>"), std::string::npos)
      << "the std include block must be emitted without --import-std";
  EXPECT_NE(out.find("import lib;"), std::string::npos);
  EXPECT_NE(out.find("#include \"lib_macros.h\""), std::string::npos);
}

TEST(ConsumerRewrite, SkipsExistingStdIncludes) {
  // An include the consumer already has must not be duplicated in the block.
  ConsumerRewriteOptions cfg;
  cfg.include_to_module = {
      {"lib/lib.h", {"lib", "lib_macros.h"}},
  };
  std::string src =
      "#include <string>\n"
      "#include \"lib/lib.h\"\n"
      "int main() {}\n";
  auto out = rewrite_consumer_source(src, cfg);
  EXPECT_EQ(out.find("#include <string>\n#include <string>"),
            std::string::npos)
      << "an existing std include must not be duplicated";
}

TEST(ConsumerRewrite, Idempotent) {
  // Rewriting an already-converted file yields the same text.
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.include_to_module = {
      {"lib/lib.h", {"lib", "lib_macros.h"}},
  };
  std::string src = "#include \"lib/lib.h\"\nint main() {}\n";
  auto once = rewrite_consumer_source(src, cfg);
  auto twice = rewrite_consumer_source(once, cfg);
  EXPECT_EQ(once, twice);
}

TEST(ConsumerRewrite, ReaddsMacroCarryingCHeaders) {
  // `import std.compat;` cannot provide the C library's macros (errno, assert,
  // pthread), so a consumer that uses them must get the C headers re-added
  // based on traced macro usage — no manual fix needed.
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.include_to_module = {
      {"lib/lib.h", {"lib", "lib_macros.h"}},
  };
  cfg.required_system_includes = {"cerrno", "cassert", "pthread.h"};
  std::string src =
      "#include \"lib/lib.h\"\n"
      "int f() { errno = EINTR; return errno; }\n"
      "void g() { assert(1); }\n"
      "void h() { pthread_mutex_t m; pthread_mutex_lock(&m); }\n";
  auto out = rewrite_consumer_source(src, cfg);
  EXPECT_NE(out.find("#include <cerrno>"), std::string::npos)
      << "errno / errno-code usage must re-add <cerrno>";
  EXPECT_NE(out.find("#include <cassert>"), std::string::npos)
      << "assert() usage must re-add <cassert>";
  EXPECT_NE(out.find("#include <pthread.h>"), std::string::npos)
      << "pthread usage must re-add <pthread.h>";
}

TEST(ConsumerRewrite, TracesSystemMacroUsageFromConsumer) {
  // The library's failure-reporting macro (see sysuse_lib/prod.h) expands to
  // code that references an internal failure class and calls `fflush(stderr)`.
  // Tracing the consumer's macro expansions records <stdio.h> (for stderr) and
  // the internal entity trace records the module owning the internal class.
  auto lib = data_path("sysuse_lib/prod.h");
  auto use = data_path("sysuse_lib/use.cc");
  auto includes = trace_consumer_system_includes({use}, {"-I", gDataDir});
  auto it = includes.find(use);
  ASSERT_NE(it, includes.end())
      << "the consumer using errno/assert/pthread must need C headers";
  EXPECT_NE(it->second.count("errno.h"), 0u)
      << "errno macro usage must need <errno.h>";
  EXPECT_NE(it->second.count("assert.h"), 0u)
      << "assert macro usage must need <assert.h>";
  EXPECT_NE(it->second.count("pthread.h"), 0u)
      << "pthread function/type usage must need <pthread.h>";
  EXPECT_NE(it->second.count("unistd.h"), 0u)
      << "ssize_t usage must need <unistd.h>";
  EXPECT_NE(it->second.count("stdio.h"), 0u)
      << "fflush(stderr) from the failure macro expansion must need <stdio.h>";
  auto mods = trace_consumer_modules({lib}, {use}, "sysuse_lib",
                                     {"-I", gDataDir});
  auto mit = mods.find(use);
  ASSERT_NE(mit, mods.end())
      << "a consumer using the library's failure macro must reference the "
         "internal class";
  EXPECT_NE(mit->second.count(lib), 0u)
      << "the internal failure class must be traced to its producer";
}

TEST(ConsumerRewrite, SsizeAndNullUsersGetUnistdAndCstddef) {
  // `import std.compat;` cannot provide ssize_t / NULL; consumers using them
  // must get <unistd.h> / <cstddef> re-added based on traced macro usage.
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.required_system_includes = {"unistd.h", "cstddef"};
  std::string src =
      "int f() { ssize_t n = static_cast<ssize_t>(0); return n == NULL; }\n";
  auto out = rewrite_consumer_source(src, cfg);
  EXPECT_NE(out.find("#include <unistd.h>"), std::string::npos)
      << "ssize_t usage (even without a trailing `(`) must re-add <unistd.h>";
  EXPECT_NE(out.find("#include <cstddef>"), std::string::npos)
      << "NULL usage must re-add <cstddef>";
}

TEST(ConsumerRewrite, ImportStdKeepsMacroCarryingClimits) {
  // `<climits>` defines INT_MAX / CHAR_BIT as macros that `import
  // std.compat;` cannot provide, so the include must not be dropped even when
  // the C++ stdlib headers are replaced by import std.
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  std::string src =
      "#include <climits>\n"
      "int f() { return INT_MAX; }\n";
  auto out = rewrite_consumer_source(src, cfg);
  EXPECT_NE(out.find("#include <climits>"), std::string::npos)
      << "the macro-carrying <climits> wrapper must be kept under import std";
  EXPECT_NE(out.find("import std.compat;"), std::string::npos)
      << "import std.compat must still be emitted";
}

TEST(ConsumerRewrite, AddsTracedInternalModuleImports) {
  // Internal entities a consumer references (e.g. a matcher function defined
  // in a sub-module) are exported from their defining sub-modules but not
  // re-exported by the
  // wrapper; the consumer must import the owning modules (traced from its
  // source against the library headers).
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.include_to_module = {
      {"mock_lib/mock_lib.h", {"mock_lib", "mock_lib_macros.h"}},
  };
  cfg.traced_imports = {"mock_lib.matchers"};
  std::string src =
      "#include \"mock_lib/mock_lib.h\"\n"
      "int main() { return Gen(std::make_tuple(), std::make_tuple()); }\n";
  auto out = rewrite_consumer_source(src, cfg);
  EXPECT_NE(out.find("import mock_lib.matchers;"), std::string::npos)
      << "a traced internal module must be imported";
  EXPECT_NE(out.find("import mock_lib;"), std::string::npos)
      << "the umbrella import must still be emitted";
}

TEST(ConsumerRewrite, ConditionalIncludeKeepsPreludeUnconditional) {
  // A library include can sit inside a preprocessor conditional (a
  // platform-guarded header). Emitting the whole block there would make
  // `import std;` and the re-added C headers conditional too, so every build
  // where the guard is false loses them. The guarded include's own import must
  // stay inside the guard — it is only needed when the guard holds.
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.include_to_module = {{"lib/lib.h", {"lib", "lib-macros.h"}}};
  cfg.required_system_includes = {"stdio.h"};
  cfg.traced_imports = {"lib.internal.detail"};
  std::string src =
      "#ifdef LIB_OS_MAC\n"
      "#include \"lib/lib.h\"\n"
      "#endif\n"
      "int main() { return 0; }\n";
  auto out = rewrite_consumer_source(src, cfg);

  auto guard = out.find("#ifdef LIB_OS_MAC");
  auto endif = out.find("#endif");
  ASSERT_NE(guard, std::string::npos);
  ASSERT_NE(endif, std::string::npos);
  auto inside = [&](std::string_view needle) {
    auto at = out.find(needle);
    return at != std::string::npos && at > guard && at < endif;
  };
  EXPECT_NE(out.find("import std.compat;"), std::string::npos);
  EXPECT_FALSE(inside("import std.compat;"))
      << "the std import must not be guarded by the consumer's #ifdef";
  EXPECT_FALSE(inside("#include <stdio.h>"))
      << "a re-added C header must not be guarded by the consumer's #ifdef";
  EXPECT_FALSE(inside("import lib.internal.detail;"))
      << "a traced import is needed regardless of the guard";
  EXPECT_TRUE(inside("import lib;"))
      << "the guarded include's own import must keep its guard";
}

TEST(ConsumerRewrite, ConditionalIncludeIdempotent) {
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.include_to_module = {{"lib/lib.h", {"lib", "lib-macros.h"}}};
  cfg.required_system_includes = {"stdio.h"};
  std::string src =
      "#ifdef LIB_OS_MAC\n"
      "#include \"lib/lib.h\"\n"
      "#endif\n"
      "int main() { return 0; }\n";
  auto once = rewrite_consumer_source(src, cfg);
  EXPECT_EQ(once, rewrite_consumer_source(once, cfg));
}

TEST(ConsumerRewrite, UnguardedIncludeStillCollapsesIntoOneBlock) {
  // The common case is unchanged: includes at file scope are replaced by a
  // single block at the position of the first one, guarded includes aside.
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.include_to_module = {
      {"lib/lib.h", {"lib", ""}},
      {"lib/extra.h", {"lib.extra", ""}},
      {"lib/mac.h", {"lib.mac", ""}},
  };
  std::string src =
      "#include \"lib/lib.h\"\n"
      "#include \"lib/extra.h\"\n"
      "#ifdef LIB_OS_MAC\n"
      "#include \"lib/mac.h\"\n"
      "#endif\n"
      "int main() { return 0; }\n";
  auto out = rewrite_consumer_source(src, cfg);
  auto lib = out.find("import lib;");
  auto extra = out.find("import lib.extra;");
  auto guard = out.find("#ifdef LIB_OS_MAC");
  auto mac = out.find("import lib.mac;");
  ASSERT_NE(lib, std::string::npos);
  ASSERT_NE(extra, std::string::npos);
  ASSERT_NE(mac, std::string::npos);
  EXPECT_LT(lib, guard) << "file-scope imports stay in the leading block";
  EXPECT_LT(extra, guard);
  EXPECT_GT(mac, guard) << "the guarded import stays inside its guard";
}

TEST(ConsumerRewrite, DemodularizeRestoresLibraryIncludes) {
  // A consumer converted by an earlier pass no longer parses: `import lib;`
  // needs a BMI that does not exist while the library is still being
  // converted, so the parse dies with "module 'lib' not found" and every later
  // pass silently loses that consumer's traced imports. Reconstructing the
  // pre-module source from the same include->module map makes it parseable
  // again, which is what tracing runs on.
  std::map<std::string, ConsumerHeaderInfo> map = {
      {"lib/lib.h", {"lib", "lib-macros.h"}},
      {"lib/internal/lib-port.h", {"lib.internal.lib_port", "lib-port-macros.h"}},
  };
  std::string converted =
      "import std.compat;\n"
      "import lib;\n"
      "import lib.internal.lib_port;\n"
      "#include \"lib-macros.h\"\n"
      "#include <stdio.h>\n"
      "int main() { return 0; }\n";
  auto out = demodularize_consumer_source(converted, map);
  EXPECT_NE(out.find("#include \"lib/lib.h\""), std::string::npos)
      << "a library module import must be restored to its header include";
  EXPECT_NE(out.find("#include \"lib/internal/lib-port.h\""), std::string::npos)
      << "a sub-module import must be restored to its header include";
  EXPECT_EQ(out.find("import lib;"), std::string::npos)
      << "no unresolvable module import may remain";
  EXPECT_EQ(out.find("import std.compat;"), std::string::npos)
      << "the std module import must not remain either";
  EXPECT_NE(out.find("#include <vector>"), std::string::npos)
      << "import std must be restored to the standard include block";
  EXPECT_EQ(out.find("#include \"lib-macros.h\""), std::string::npos)
      << "generated macro headers do not exist pre-conversion and must be "
         "dropped";
  EXPECT_NE(out.find("#include <stdio.h>"), std::string::npos)
      << "the consumer's own system includes must be preserved";
  EXPECT_NE(out.find("int main() { return 0; }"), std::string::npos)
      << "the consumer's code must be preserved";
}

TEST(ConsumerRewrite, DemodularizeDropsAnyGeneratedMacroHeader) {
  // A consumer shared by two libraries carries the macro headers of both, but
  // one pass only knows its own library's map. Those headers are an output of
  // the conversion and need not exist while it runs, so a leftover include of
  // one is fatal to the parse ("file not found"). The transform recognises the
  // generated naming (`<stem>-macros.h` / `<stem>_macros.h`) and drops them all.
  // It only ever feeds a parse, so dropping a same-named header of the
  // consumer's own would at worst make that parse slightly less complete.
  std::map<std::string, ConsumerHeaderInfo> map = {
      {"lib/lib.h", {"lib", "lib-macros.h"}}};
  std::string converted =
      "import lib;\n"
      "#include \"lib-macros.h\"\n"
      "#include \"other/other-macros.h\"\n"
      "#include \"sibling/sibling_macros.h\"\n"
      "#include \"real/header.h\"\n"
      "int main() { return 0; }\n";
  auto out = demodularize_consumer_source(converted, map);
  EXPECT_EQ(out.find("lib-macros.h"), std::string::npos)
      << "this pass's own macro header must go";
  EXPECT_EQ(out.find("other/other-macros.h"), std::string::npos)
      << "a sibling library's macro header must go too";
  EXPECT_EQ(out.find("sibling/sibling_macros.h"), std::string::npos)
      << "the underscore naming variant must go as well";
  EXPECT_NE(out.find("#include \"real/header.h\""), std::string::npos)
      << "an ordinary quoted include must be kept";
}

TEST(ConsumerRewrite, DemodularizeDropsUnmappedModuleImports) {
  // Traced internal sub-module imports have no include form of their own (the
  // entities come from the library headers being restored), and a sibling
  // library's modules are not in this pass's map. Neither may be left behind
  // as an unresolvable import.
  std::map<std::string, ConsumerHeaderInfo> map = {
      {"lib/lib.h", {"lib", ""}},
  };
  std::string converted =
      "import lib;\n"
      "import lib.internal.detail;\n"
      "import other_lib;\n"
      "int main() { return 0; }\n";
  auto out = demodularize_consumer_source(converted, map);
  EXPECT_NE(out.find("#include \"lib/lib.h\""), std::string::npos);
  EXPECT_EQ(out.find("import"), std::string::npos)
      << "every import must be gone: what cannot be mapped must be dropped";
}

TEST(ConsumerRewrite, DemodularizeIsInverseOfRewrite) {
  // Round trip: rewriting a consumer and then demodularizing it restores the
  // library includes the original had, so a later pass traces the same source
  // the first pass did.
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.include_to_module = {{"lib/lib.h", {"lib", "lib-macros.h"}}};
  std::string src =
      "#include \"lib/lib.h\"\n"
      "int main() { return 0; }\n";
  auto converted = rewrite_consumer_source(src, cfg);
  auto restored = demodularize_consumer_source(converted, cfg.include_to_module);
  EXPECT_NE(restored.find("#include \"lib/lib.h\""), std::string::npos)
      << "the library include the original consumer had must be back";
  EXPECT_EQ(restored.find("import "), std::string::npos)
      << "no module import may survive the inverse transform";
}

TEST(ConsumerRewrite, DemodularizeLeavesUnconvertedSourceAlone) {
  // A consumer that was never converted must pass through untouched, so the
  // transform is safe to apply to every consumer regardless of pass order.
  std::map<std::string, ConsumerHeaderInfo> map = {{"lib/lib.h", {"lib", ""}}};
  std::string src =
      "#include \"lib/lib.h\"\n"
      "#include <vector>\n"
      "int main() { return 0; }\n";
  EXPECT_EQ(demodularize_consumer_source(src, map), src);
}

TEST(ConsumerRewrite, BuildIncludeModuleMap) {
  // The include form a consumer writes (twocons_lib/producer.h) must map to
  // the derived module name (twocons_lib.producer).
  auto producer = data_path("twocons_lib/producer.h");
  auto map = build_include_module_map(
      {producer}, "twocons_lib", {"-I", gDataDir},
      /*hyphen_macros=*/false, /*headers_with_macros=*/{});
  auto it = map.find("twocons_lib/producer.h");
  ASSERT_NE(it, map.end())
      << "the include form under the -I directory must be mapped";
  EXPECT_EQ(it->second.module, "twocons_lib.producer");
  EXPECT_TRUE(it->second.macro_header.empty())
      << "a header with no macros must have no macro file";
}

TEST(ConsumerRewrite, ReAddedSystemIncludesComeBeforeTheImports) {
  // A re-added C header is not always the C library's own: a standard library
  // ships wrappers under the C names — libstdc++'s `math.h` includes `<cmath>`.
  // Included after `import std.compat;` such a header redeclares what the
  // module already provided, and the translation unit fails inside the standard
  // library itself:
  //
  //   ext/type_traits.h: error: type alias template redefinition with
  //                      different types
  //
  // Seen first, the textual declarations are in place before the module arrives
  // and the two agree.
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.include_to_module = {{"lib/lib.h", {"lib", ""}}};
  cfg.required_system_includes = {"math.h"};
  std::string src =
      "#include \"lib/lib.h\"\n"
      "double f() { return NAN; }\n";

  auto out = rewrite_consumer_source(src, cfg);
  auto inc = out.find("#include <math.h>");
  auto imp = out.find("import std.compat;");
  ASSERT_NE(inc, std::string::npos);
  ASSERT_NE(imp, std::string::npos);
  EXPECT_LT(inc, imp)
      << "a re-added C header must precede the standard library import";
}

TEST(ConsumerRewrite, TracesOutsideHeadersTheImportCannotSupply) {
  // The consumer includes only the library header and takes `outside::Gadget`
  // and OUTSIDE_TAG through it. Converting the library does not convert the
  // outside header: the library's module units include it in their global
  // module fragment, where its declarations attach to the global module and
  // are invisible to anyone importing the module. The macro does not cross a
  // module boundary at all.
  //
  // So the include the consumer never wrote has to be written for it:
  //
  //   error: missing '#include "boost/assert/source_location.hpp"';
  //          'source_location' must be declared before it is used
  //   error: use of undeclared identifier 'BOOST_CURRENT_LOCATION'
  auto use = data_path("outsidelib/use.cc");
  auto lib = data_path("outsidelib/api.h");
  auto needed = trace_consumer_outside_includes({use}, {lib}, {"-I", gDataDir});
  auto it = needed.find(use);
  ASSERT_NE(it, needed.end())
      << "a consumer naming entities from an unconverted header needs it";
  EXPECT_NE(it->second.count("outside/gadget.h"), 0u)
      << "spelled as the include path, not as a bare file name — a bare "
         "`gadget.h` resolves to nothing";
}

TEST(ConsumerRewrite, DoesNotAskForTheLibraryHeadersItImports) {
  // The other half: what the module DOES provide must not come back as an
  // include. Re-adding a library header alongside the import of the same
  // module declares everything in it twice, once in the global module and
  // once attached to the module.
  auto use = data_path("outsidelib/use.cc");
  auto lib = data_path("outsidelib/api.h");
  auto needed = trace_consumer_outside_includes({use}, {lib}, {"-I", gDataDir});
  auto it = needed.find(use);
  ASSERT_NE(it, needed.end());
  for (const auto &inc : it->second)
    EXPECT_EQ(inc.find("outsidelib/"), std::string::npos)
        << "the library's own header must not be re-added: " << inc;
}

TEST(ConsumerRewrite, TracesTheOutsideHeadersTheLibraryInterfaceLeansOn) {
  // A module's global module fragment is pruned to what is decl-reachable from
  // its exported declarations. An outside TYPE named in the interface survives;
  // an outside operator found only by ADL, when some template using it is
  // instantiated in the consumer's translation unit, does not:
  //
  //   error: invalid operands to binary expression
  //          ('const variant2::variant<...>' and the same)
  //   note: candidate function not viable: operator==(monostate, monostate)
  //
  // — variant.hpp plainly visible, and the one operator that matters gone. So
  // the outside headers the library's own headers pull in have to reach the
  // consumer, which is exactly what it used to get transitively before the
  // include became an import.
  auto needed = trace_library_outside_includes(
      {data_path("outsidelib/holder.h"), data_path("outsidelib/api.h")},
      {"-I", gDataDir});
  EXPECT_NE(needed.count("outside/gadget.h"), 0u)
      << "the header carrying the ADL-only operator must reach the consumer";
}

TEST(ConsumerRewrite, StillReportsAHeaderSomeModuleProvides) {
  // Whether a module actually provides one of these is decided at BUILD time,
  // by the provider's own guard. Dropping it from the trace answers that
  // question at conversion time and gets it wrong for every build with the
  // guard off: nothing imports the module, the library's units include the
  // header in their global module fragment, and pruning takes the ADL-only
  // operators away again.
  //
  // So it is reported, and the caller emits both answers — the import under
  // the guard, the include under its negation.
  ModuleReplacements provided = {
      ModuleReplacement{.module = "outside.gadget",
                        .headers = {"outside/gadget.h"},
                        .guard = "OUTSIDE_IMPORT_MODULES"}};
  auto needed = trace_library_outside_includes(
      {data_path("outsidelib/holder.h")}, {"-I", gDataDir}, provided,
      "OUTSIDELIB_USE_MODULES");
  EXPECT_NE(needed.count("outside/gadget.h"), 0u)
      << "a header a module MIGHT provide is still one the consumer may need";
}

TEST(ConsumerRewrite, EmitsImportAndIncludeForAProvidedHeader) {
  // The pair the guard chooses between, so one generated tree serves a build
  // with the converted dependency and a build without it.
  ConsumerRewriteOptions cfg;
  cfg.include_to_module = {{"outsidelib/holder.h", {"outsidelib.holder", ""}}};
  cfg.required_module_includes = {
      ModuleReplacement{.module = "outside.gadget",
                        .headers = {"outside/gadget.h"},
                        .guard = "OUTSIDE_IMPORT_MODULES",
                        .carries_macros_file = true}};
  auto out = rewrite_consumer_source(
      "#include \"outsidelib/holder.h\"\nint main() { return 0; }\n", cfg);
  EXPECT_NE(out.find("#if defined(OUTSIDE_IMPORT_MODULES)"), std::string::npos)
      << "got:\n" << out;
  EXPECT_NE(out.find("import outside.gadget;"), std::string::npos);
  EXPECT_NE(out.find("#include <outside/gadget.h>"), std::string::npos)
      << "and the include for the build that has not switched it on";
  EXPECT_NE(out.find("outside/gadget_macros.h"), std::string::npos)
      << "with the provider's macros file, which the module cannot carry";
}

TEST(ConsumerRewrite, LeavesOutAnIncludeTheLibraryOnlyMakesConditionally) {
  // A header the library reaches for only on another platform is not the
  // consumer's to include. Handed over unconditionally it is handed to every
  // platform, and the ones it was never meant for stop compiling:
  //
  //   boost/winapi/basic_types.hpp:38:3: error: "Win32 functions not available"
  //
  // The conversion's own `<PREFIX>_USE_MODULES` guard does not count: every
  // generated header wraps its includes in it, and it says nothing about
  // whether the include was conditional to begin with.
  auto needed = trace_library_outside_includes(
      {data_path("outsidelib/holder.h")}, {"-I", gDataDir},
      /*provided=*/{}, "OUTSIDELIB_USE_MODULES");
  EXPECT_EQ(needed.count("outside/winonly.h"), 0u)
      << "a conditional include must not be handed on unconditionally";
  EXPECT_NE(needed.count("outside/gadget.h"), 0u)
      << "while one guarded only by the conversion's own macro still must be";
}

TEST(ConsumerRewrite, ReadsPastAHeadersOwnIncludeGuard) {
  // An include guard wraps the whole file and is true exactly once; it says
  // nothing about the includes inside it. Counted as a condition it makes
  // every include in every guarded header look conditional — which is every
  // header there is — and the trace reports nothing at all.
  auto needed = trace_library_outside_includes(
      {data_path("outsidelib/guarded.h")}, {"-I", gDataDir}, /*provided=*/{},
      "OUTSIDELIB_USE_MODULES");
  EXPECT_NE(needed.count("outside/gadget.h"), 0u)
      << "an include guard must not hide what the header includes";
}

TEST(ConsumerRewrite, OnlyAsksForIncludesThatResolveOnTheSearchPath) {
  // The re-added includes are emitted `#include <...>`, so a spelling that
  // only works relative to the file that wrote it is worse than nothing.
  // Clang will happily suggest one — it is the shortest way to name the file
  // from the consumer — and the result does not compile:
  //
  //   test/production.cc:35:10: fatal error: production.h: No such file
  //     35 | #include <production.h>
  auto use = data_path("outsidelib/nested/use_sidecar.cc");
  auto lib = data_path("outsidelib/api.h");
  auto needed = trace_consumer_outside_includes({use}, {lib}, {"-I", gDataDir});
  auto it = needed.find(use);
  ASSERT_NE(it, needed.end())
      << "this consumer does reach an outside header, so a trace that found "
         "nothing is the thing going wrong, not a case to wave through";
  EXPECT_EQ(it->second.count("sidecar.h"), 0u)
      << "a bare name resolves against no search directory";
  for (const auto &inc : it->second)
    EXPECT_NE(inc.find('/'), std::string::npos)
        << "every re-added include must be search-path relative: " << inc;
}

TEST(ConsumerRewrite, KeepsAMacroOnlyHeaderReachableFromAConsumerHeader) {
  // A header that defines nothing but a macro has no declarations to export,
  // so nothing about it crosses a module boundary. A consumer that used the
  // macro must still be able to: either the include stays, or the macros file
  // that carries it arrives instead.
  //
  //   production.h:44:3: error: 'FRIEND_TEST' has not been declared
  //   production.h:44:3: error: ISO C++ forbids declaration of 'FRIEND_TEST'
  //                             with no type
  ConsumerRewriteOptions cfg;
  cfg.is_header = true;
  cfg.include_to_module = {
      {"macroonly/friend_macro.h",
       {"macroonly.friend_macro", "macroonly/friend_macro_macros.h"}}};
  auto out = rewrite_consumer_source(
      read_file(data_path("macroonly/user.h")), cfg);
  bool has_macros_header =
      out.find("macroonly/friend_macro_macros.h") != std::string::npos;
  bool kept_include =
      out.find("#include \"macroonly/friend_macro.h\"") != std::string::npos;
  EXPECT_TRUE(has_macros_header || kept_include)
      << "the macro has to reach the consumer somehow; got:\n"
      << out;
}

TEST(ConsumerRewrite, ProbesForAMacrosFileItCouldNotResolve) {
  // The macros file is found by asking the include path for it, and a path
  // that answers differently at rewrite time than at build time leaves the
  // consumer with an import and nothing else — every macro it used to get
  // from that header is gone:
  //
  //   production.h:44:3: error: 'FRIEND_TEST' has not been declared
  //
  // The conversion writes a macros file whenever a header defines macros, so
  // "not found" is worth a second question rather than a conclusion. Asked
  // with __has_include, a file that is there is used and one that is not
  // costs nothing.
  ConsumerRewriteOptions cfg;
  cfg.is_header = true;
  cfg.include_to_module = {
      {"macroonly/friend_macro.h", {"macroonly.friend_macro", ""}}};
  auto out = rewrite_consumer_source(
      read_file(data_path("macroonly/user.h")), cfg);
  EXPECT_NE(out.find("import macroonly.friend_macro;"), std::string::npos);
  EXPECT_NE(out.find("__has_include(\"macroonly/friend_macro_macros.h\")"),
            std::string::npos)
      << "ask for the macros file rather than assume it is not there; got:\n"
      << out;
}

TEST(ConsumerRewrite, IgnoresALibraryHeaderThatCannotBeReadHere) {
  // Every library header is parsed on its own to see what it includes, which
  // reaches headers no build on this platform ever includes. Their includes
  // are unconditional WITHIN them — the condition is on whoever includes them
  // — so nothing inside the file says "not here", and handing those on gives
  // every consumer a header for another platform:
  //
  //   boost/winapi/basic_types.hpp:38:3: error: "Win32 functions not available"
  //
  // A header that cannot be read here cannot say what a consumer needs.
  auto needed = trace_library_outside_includes(
      {data_path("outsidelib/other_platform.h")}, {"-I", gDataDir},
      /*provided=*/{}, "OUTSIDELIB_USE_MODULES");
  EXPECT_EQ(needed.count("outside/platform_only.h"), 0u)
      << "a header that fails to parse contributes nothing";
  EXPECT_TRUE(needed.empty()) << "nothing at all, in fact";
}

TEST(ConsumerRewrite, StillReadsTheHeadersThatDoParse) {
  // And the ones that do parse are unaffected by a sibling that does not.
  auto needed = trace_library_outside_includes(
      {data_path("outsidelib/other_platform.h"),
       data_path("outsidelib/holder.h")},
      {"-I", gDataDir}, /*provided=*/{}, "OUTSIDELIB_USE_MODULES");
  EXPECT_NE(needed.count("outside/gadget.h"), 0u)
      << "one unreadable header must not silence the rest";
  EXPECT_EQ(needed.count("outside/platform_only.h"), 0u);
}

// The line a compiler reports for `needle` in `text`, counting the way one
// does: each line advances the counter, and `#line N` sets the next line to N.
static std::size_t reported_line_of(const std::string &text,
                                    std::string_view needle) {
  std::size_t line = 1;
  for (std::size_t pos = 0; pos < text.size();) {
    auto nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    auto content = std::string_view(text).substr(pos, nl - pos);
    if (content.contains(needle)) return line;
    auto trimmed = content.substr(std::min(content.find_first_not_of(" \t"),
                                           content.size()));
    if (trimmed.starts_with("#line ")) {
      line = std::strtoul(std::string(trimmed.substr(6)).c_str(), nullptr, 10);
    } else {
      ++line;
    }
    pos = nl + 1;
  }
  return 0;
}

TEST(ConsumerRewrite, KeepsEveryLineWhereItWas) {
  // The rewrite adds imports and moves includes, so it restores the numbering
  // with `#line`. Off by one there is invisible until something reports a
  // source location, and then every one of them is wrong:
  //
  //   ec_location_test.cpp(36): test 'ec.location().line() == 27'
  //                             ('26' == '27') failed
  auto src = read_file(data_path("lineno/use.cc"));
  ASSERT_EQ(reported_line_of(src, "LINENO_MARKER"), 10u)
      << "the fixture itself must have the marker on line 10";

  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.include_to_module = {{"lineno/lib.h", {"lineno.lib", ""}}};
  auto out = rewrite_consumer_source(src, cfg);
  EXPECT_EQ(reported_line_of(out, "LINENO_MARKER"), 10u)
      << "the marker has to still report line 10; got:\n"
      << out;
}

TEST(ConsumerRewrite, SpellsAMacrosFileTheWayTheLibraryNamesItsFiles) {
  // A library whose headers are hyphenated gets hyphenated macros files, and a
  // consumer asked to include the underscored name asks for a file nobody
  // wrote. The macro it came for is then simply missing:
  //
  //   production.h:41:3: error: a type specifier is required for all
  //                             declarations
  //     41 |   FRIEND_TEST(PrivateCodeTest, CanAccessPrivateMembers);
  ConsumerRewriteOptions cfg;
  cfg.hyphen_macros = true;
  cfg.include_to_module = {{"lib/thing-here.h", {"lib.thing_here", ""}}};
  auto out = rewrite_consumer_source(
      "#include \"lib/thing-here.h\"\nint main() { return 0; }\n", cfg);
  EXPECT_NE(out.find("lib/thing-here-macros.h"), std::string::npos)
      << "the separator the library uses, not the default; got:\n"
      << out;
  EXPECT_EQ(out.find("thing-here_macros.h"), std::string::npos);
}

TEST(ConsumerRewrite, SpellsAProvidersMacrosFileTheSameWay) {
  // The same for a module that provides headers: its macros file is named the
  // way that library names files, not the way this one does.
  ConsumerRewriteOptions cfg;
  cfg.hyphen_macros = true;
  cfg.module_replacements = {
      ModuleReplacement{.module = "dep.thing_here",
                        .headers = {"dep/thing-here.h"},
                        .guard = "DEP_IMPORT_MODULES",
                        .carries_macros_file = true}};
  auto out = rewrite_consumer_source(
      "#include \"dep/thing-here.h\"\nint main() { return 0; }\n", cfg);
  EXPECT_NE(out.find("dep/thing-here-macros.h"), std::string::npos)
      << "got:\n"
      << out;
}

TEST(ConsumerRewrite, DoesNotAlsoHoistAHeaderReplacedWhereTheSourceIncludesIt) {
  // The consumer needs a header the library stopped pulling in, so it is
  // hoisted to the top; the same header has a module replacement, so the
  // include the source wrote is turned into the guarded pair in place. Both
  // at once hands the consumer the textual declarations first, and they are
  // the global module's while the import's are the provider's:
  //
  //   error: 'boost::detail::lwt_long_type<true>::type' from module
  //          'boost.core.lightweight_test' is not present in definition of
  //          'boost::detail::lwt_long_type<true>' provided earlier
  //
  // The guarded pair answers both builds, so the hoisted copy is not the
  // consumer's to add.
  ConsumerRewriteOptions cfg;
  cfg.required_system_includes = {"outside/gadget.h"};
  cfg.module_replacements = {
      ModuleReplacement{.module = "outside.gadget",
                        .headers = {"outside/gadget.h"},
                        .guard = "OUTSIDE_IMPORT_MODULES"}};
  auto out = rewrite_consumer_source(
      "#include <outside/gadget.h>\nint main() { return 0; }\n", cfg);
  auto guard = out.find("#if defined(OUTSIDE_IMPORT_MODULES)");
  ASSERT_NE(guard, std::string::npos) << "no guarded pair; got:\n" << out;
  EXPECT_EQ(out.rfind("#include <outside/gadget.h>", guard), std::string::npos)
      << "the header must not also be hoisted above the pair; got:\n"
      << out;
}

TEST(ConsumerRewrite, StillProvidesAReplacedHeaderTheSourceNeverIncluded) {
  // The other half of the rule above. This header is hoisted because the
  // library stopped pulling it in, and the source never wrote an include for
  // it — so there is no pair standing in place to hand it over. Dropping it
  // for having a replacement leaves the consumer without it altogether:
  //
  //   error: use of undeclared identifier 'BOOST_CURRENT_FUNCTION'
  //
  // It is owed, and owed as the pair, so the import serves the build that has
  // the module and the include the build that has not.
  ConsumerRewriteOptions cfg;
  cfg.required_system_includes = {"outside/gadget.h"};
  cfg.module_replacements = {
      ModuleReplacement{.module = "outside.gadget",
                        .headers = {"outside/gadget.h"},
                        .guard = "OUTSIDE_IMPORT_MODULES"}};
  auto out = rewrite_consumer_source("int main() { return 0; }\n", cfg);
  EXPECT_NE(out.find("import outside.gadget;"), std::string::npos)
      << "the module for the build that has it; got:\n"
      << out;
  EXPECT_NE(out.find("#include <outside/gadget.h>"), std::string::npos)
      << "and the include for the build that has not; got:\n"
      << out;
}

TEST(ConsumerRewrite, ReadsStringHAheadOfEverythingElseTheBlockBrings) {
  // <string.h> is read for gcc's sake, and it has to be read before any
  // import — that is the whole of what it is for. The block it sits in also
  // carries other consumers' headers, and one of those imports std itself,
  // so last in the block is still below an import:
  //
  //   string.h:105: error: redefinition of 'void* memchr(void*, int, size_t)'
  //   note: 'void* memchr(void*, int, size_t)' previously defined here
  //   ... of module std.compat, imported at <a header the block brought in>
  //
  // First in the block is the only place that holds.
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.is_header = false;
  cfg.required_system_includes = {"outside/gadget.h"};
  auto out = rewrite_consumer_source(
      "#include \"peer.h\"\nint main() { return 0; }\n", cfg);
  auto sh = out.find("#include <string.h>");
  ASSERT_NE(sh, std::string::npos) << "not read at all; got:\n" << out;
  auto other = out.find("#include <outside/gadget.h>");
  ASSERT_NE(other, std::string::npos) << "got:\n" << out;
  EXPECT_LT(sh, other) << "string.h must lead the block, since anything else "
                          "in it may import std; got:\n"
                       << out;
}

TEST(ConsumerRewrite, LiftsAStdProvidedCHeaderAboveTheBlocksOwnHeaders) {
  // <time.h> and its kind declare what the std module also declares, so a
  // translation unit that reads one after importing std defines those twice:
  //
  //   bits/types/struct_tm.h:7: error: redefinition of 'struct tm'
  //   note: previous definition, of module std.compat, imported at
  //         gmock-matchers_test.h:40
  //
  // The import is not in this file. The block hands over another consumer's
  // header, that header imports std itself, and a C header written after it
  // in the block is already too late. They go to the front of the block, as
  // <string.h> does and for the same reason.
  ConsumerRewriteOptions cfg;
  cfg.import_std = true;
  cfg.is_header = false;
  // Sorted, so the peer header is handed over first and the C header second —
  // which is the order that breaks.
  cfg.required_system_includes = {"peer/other.h", "time.h"};
  auto out = rewrite_consumer_source("int main() { return 0; }\n", cfg);
  auto th = out.find("#include <time.h>");
  auto peer = out.find("#include <peer/other.h>");
  ASSERT_NE(th, std::string::npos) << "dropped altogether; got:\n" << out;
  ASSERT_NE(peer, std::string::npos) << "got:\n" << out;
  EXPECT_LT(th, peer)
      << "a std-provided C header must lead the block, since anything else in "
         "it may import std; got:\n"
      << out;
}

TEST(ConsumerRewrite, LeavesAThirdPartyIncludeBelowTheImportItFollowed) {
  // The block sits where the first replaced include stood, and system headers
  // are lifted into it so none is read after an import. A third-party header
  // is not one of those: lifting it moves it above the import it used to
  // follow, and the library it belongs to is read in an order its own headers
  // never arranged for —
  //
  //   boost/mpl/aux_/integral_wrapper.hpp:42: error: unknown type name
  //                                          'AUX_WRAPPER_VALUE_TYPE'
  //
  // which is an x-macro header, read once per wrapper with the wrapper's
  // macros defined around it. It keeps its place.
  ConsumerRewriteOptions cfg;
  cfg.include_to_module = {{"lib/thing.h", {"lib.thing", ""}}};
  auto out = rewrite_consumer_source(
      "#include <lib/thing.h>\n#include <other/aux.h>\nint main() { return 0; }\n",
      cfg);
  auto imp = out.find("import lib.thing;");
  auto other = out.find("#include <other/aux.h>");
  ASSERT_NE(imp, std::string::npos) << "got:\n" << out;
  ASSERT_NE(other, std::string::npos) << "got:\n" << out;
  EXPECT_LT(imp, other)
      << "the import stands where the include it replaced stood, above what "
         "followed it; got:\n"
      << out;
}

TEST(ConsumerRewrite, CreditsAFragmentsDeclarationToTheHeaderThatOwnsIt) {
  // A declaration is credited to the file it is written in, and some files are
  // not headers: an x-macro fragment carries no include guard and names terms
  // its includer defines around it. Handed to a consumer as an include, it is
  // read once, alone, with none of them —
  //
  //   bind/detail/bind_cc.hpp:16: error: unknown type name 'BOOST_BIND_ST'
  //   bind/detail/bind_cc.hpp:16: error: use of undeclared identifier '_bi'
  //
  // and the second of those because the fragment is read outside the namespace
  // its includer opened. The header that owns it is the one to name.
  auto use = data_path("outsidelib/xmac/use_xmac.cc");
  auto lib = data_path("outsidelib/api.h");
  auto needed = trace_consumer_outside_includes({use}, {lib}, {"-I", gDataDir});
  auto it = needed.find(use);
  ASSERT_NE(it, needed.end())
      << "the consumer reaches an outside declaration, so something is owed";
  EXPECT_EQ(it->second.count("outsidelib/xmac/body.hpp"), 0u)
      << "a fragment with no guard of its own cannot be included alone";
  EXPECT_NE(it->second.count("outsidelib/xmac/wrapper.hpp"), 0u)
      << "the header that defines its terms and reads it is what a consumer "
         "includes";
}

TEST(ConsumerRewrite, KeepsEveryLineWhereItWasWhenIncludesFollowTheLibrarysOwn) {
  // The library include is replaced where it stands, and it restores the count
  // with a `#line` naming the source line after itself. The includes BELOW it
  // are taken up into the block above it, and those lines are gone from the
  // body too — so the line after the replacement is not the next one, it is
  // the next one nobody took.
  //
  // Counted the first way, everything below reports low by however many were
  // taken, and only something that asks where it is ever notices:
  //
  //   source_location_test4.cpp(35): test 's_loc.line() == 24' ('22' == '24')
  //                                  failed in function 'int main()'
  auto src = read_file(data_path("lineno/use2.cc"));
  ASSERT_EQ(reported_line_of(src, "LINENO_MARKER2"), 11u)
      << "the fixture itself must have the marker on line 11";

  // The quoted include stays where it is and sets the point the block goes
  // in; the two standard headers BELOW it are lifted into that block, so
  // their lines leave the body from underneath the line that stayed. One
  // `#line` after the block cannot say both where the kept line is and where
  // the first line after the lifted ones is.
  ConsumerRewriteOptions cfg;
  auto out = rewrite_consumer_source(src, cfg);
  EXPECT_EQ(reported_line_of(out, "LINENO_MARKER2"), 11u)
      << "the marker has to still report line 11; got:\n"
      << out;
}
