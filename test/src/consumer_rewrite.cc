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
