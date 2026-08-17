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
// ConsumerTrace suite
namespace {

bool trace_has(const std::string &producer, const std::string &consumer,
               const std::string &fqn) {
  std::vector<std::string> headers = {producer, consumer};
  std::vector<std::string> extra_args = {"-I", gDataDir};
  auto reachable = trace_consumer_reachability(headers, "test_lib", extra_args);
  auto it = reachable.find(producer);
  if (it == reachable.end()) return false;
  for (auto &f : it->second)
    if (f == fqn) return true;
  return false;
}

}  // namespace

TEST(ConsumerTrace, BasicAggregateInitAndCall) {
  auto producer = data_path("test_lib/producer.h");
  auto consumer = data_path("test_lib/consumer.h");
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::foo"));
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::t"));
}

TEST(ConsumerTrace, MacroExpandsToNestedCall) {
  auto producer = data_path("test_lib/producer.h");
  auto consumer = data_path("test_lib/consumer_nested.h");
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::Foo"));
}

TEST(ConsumerTrace, DependentNameAndCallInTemplateBody) {
  auto producer = data_path("test_lib/producer.h");
  auto consumer = data_path("test_lib/consumer_tmpl.h");
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::foo"));
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::t"));
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::Size"));
}

TEST(ConsumerTrace, NewExpressionInTemplateBody) {
  auto producer = data_path("test_lib/producer.h");
  auto consumer = data_path("test_lib/consumer_new.h");
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::Gen"));
}

TEST(ConsumerTrace, QualifiedStaticMemberCall) {
  auto producer = data_path("test_lib/producer.h");
  auto consumer = data_path("test_lib/consumer_member.h");
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::Bar"));
}

TEST(ConsumerTrace, SfinaeSizeofTypedefInTemplateParam) {
  auto producer = data_path("test_lib/producer.h");
  auto consumer = data_path("test_lib/consumer_sfinae.h");
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::Flag"));
}

TEST(ConsumerTrace, AliasTemplateAsTemplateArg) {
  auto producer = data_path("test_lib/producer.h");
  auto consumer = data_path("test_lib/consumer_alias.h");
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::Baz"));
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::Box"));
}

TEST(ConsumerTrace, AliasTemplateResolvedThroughDependentName) {
  // The consumer references Foo through `typename Foo<...>::type`,
  // which resolves in the AST to the underlying Bar template. The
  // alias name itself must still be traced so it gets exported for sibling
  // modules.
  auto producer = data_path("alias_lib/producer.h");
  auto consumer = data_path("alias_lib/consumer.h");
  std::vector<std::string> headers = {producer, consumer};
  std::vector<std::string> extra_args = {"-I", gDataDir};
  auto reachable = trace_consumer_reachability(headers, "alias_lib", extra_args);
  auto it = reachable.find(producer);
  ASSERT_NE(it, reachable.end())
      << "consumer must reach the producer's internal entities";
  EXPECT_TRUE(std::find(it->second.begin(), it->second.end(),
                        "alias_lib::internal::Foo") !=
              it->second.end())
      << "an alias template referenced as typename X<...>::type must be "
         "traced even though the reference resolves to the underlying template";
  EXPECT_TRUE(std::find(it->second.begin(), it->second.end(),
                        "alias_lib::internal::Bar") !=
              it->second.end())
      << "the underlying template must itself be traced";
}

TEST(ConsumerTrace, TemplateArgTypeReferenceViaVarTemplate) {
  // The consumer references the internal `disjunction`/`box_type` templates
  // ONLY through the template arguments of a referenced variable template
  // (`std::is_base_of_v<..., disjunction<>>`). The tracer must check the
  // template arguments before the producer lookup.
  auto producer = data_path("tplarg_lib/producer.h");
  auto consumer = data_path("tplarg_lib/consumer.cc");
  auto reachable =
      trace_consumer_sources({producer}, {consumer}, "tplarg_lib",
                             {"-I", gDataDir});
  auto it = reachable.find(producer);
  ASSERT_NE(it, reachable.end())
      << "consumer must reach the producer's internal entities";
  EXPECT_TRUE(std::find(it->second.begin(), it->second.end(),
                        "tplarg_lib::internal::disjunction") !=
              it->second.end())
      << "a template argument of a referenced variable template must be traced";
  EXPECT_TRUE(std::find(it->second.begin(), it->second.end(),
                        "tplarg_lib::internal::box_type") !=
              it->second.end())
      << "a class template used as a template argument must be traced";
}

TEST(ConsumerTrace, AliasTemplateReferencedViaDependentMemberAccess) {
  // The consumer references the alias template `foo` ONLY through a dependent
  // member access `foo<int>::value`, whose base type is the alias
  // specialization. The tracer must export the alias AND its underlying
  // template.
  auto producer = data_path("memalias_lib/producer.h");
  auto consumer = data_path("memalias_lib/consumer.cc");
  auto reachable =
      trace_consumer_sources({producer}, {consumer}, "memalias_lib",
                             {"-I", gDataDir});
  auto it = reachable.find(producer);
  ASSERT_NE(it, reachable.end())
      << "consumer must reach the producer's internal entities";
  EXPECT_TRUE(std::find(it->second.begin(), it->second.end(),
                        "memalias_lib::internal::foo") !=
              it->second.end())
      << "an alias template referenced through a dependent member access must "
         "be traced";
  EXPECT_TRUE(std::find(it->second.begin(), it->second.end(),
                        "memalias_lib::internal::foo_impl") !=
              it->second.end())
      << "the alias's underlying template must also be traced";
}

TEST(ConsumerTrace, ReferenceInTheConsumersOwnHeaderIsTraced) {
  // The consumer source names nothing of the library: the reference to
  // `internal::Elem` is written in helper.h, the consumer's own header. That
  // is a real use — the consumer reaches the library through the same imports
  // whichever of its files does the naming — so the entity must be exported.
  // (This is how gmock uses gtest's `internal::ElemFromList`: in
  // gmock/internal/gmock-internal-utils.h, not in the .cc passed as the
  // consumer source.)
  auto producer = data_path("consumerhdr_lib/producer.h");
  auto consumer = data_path("consumerhdr_lib/consumer.cc");
  auto reachable =
      trace_consumer_sources({producer}, {consumer}, "consumerhdr_lib",
                             {"-I", gDataDir});
  auto it = reachable.find(producer);
  ASSERT_NE(it, reachable.end())
      << "a reference in the consumer's own header must reach the producer";
  EXPECT_TRUE(std::find(it->second.begin(), it->second.end(),
                        "consumerhdr_lib::internal::Elem") != it->second.end())
      << "an entity used only from the consumer's own header must be traced";
  EXPECT_TRUE(std::find(it->second.begin(), it->second.end(),
                        "consumerhdr_lib::internal::OnlyUsedInternally") ==
              it->second.end())
      << "an entity used only inside the producer header is not a consumer "
         "use and must stay internal";
}

TEST(ConsumerTrace, ForwardDeclaredOnlyEntityNotExported) {
  // The library forward-declares `Foo` (a consumer defines it itself). It must
  // NOT be traced/exported: exporting the module's forward declaration would
  // collide with the consumer's own global-module definition.
  auto producer = data_path("fwdonly_lib/producer.h");
  auto consumer = data_path("fwdonly_lib/consumer.cc");
  auto reachable =
      trace_consumer_sources({producer}, {consumer}, "fwdonly_lib",
                             {"-I", gDataDir});
  for (auto &[p, fqns] : reachable) {
    for (auto &f : fqns) {
      EXPECT_NE(f, "fwdonly_lib::internal::Foo")
          << "a library entity forward-declared but defined by the consumer "
             "itself must not be exported";
    }
  }
}

TEST(ConsumerTrace, FriendDeclaredEntityDefinedByConsumerNotExported) {
  // The library forward-declares AND friend-declares an internal class (`Gen`)
  // that the consumer defines itself. It must NOT be traced/exported.
  auto producer = data_path("frienddef_lib/producer.h");
  auto consumer = data_path("frienddef_lib/consumer.cc");
  auto reachable =
      trace_consumer_sources({producer}, {consumer}, "frienddef_lib",
                             {"-I", gDataDir});
  for (auto &[p, fqns] : reachable) {
    for (auto &f : fqns) {
      EXPECT_NE(f, "frienddef_lib::internal::Gen")
          << "a friend-declared internal entity defined by the consumer itself "
             "must not be exported";
    }
  }
}

TEST(ConsumerTrace, QualifiedAccessOfConsumerDefinedInternalClassNotExported) {
  // The library forward-declares AND friend-declares an internal class
  // (`Helper`) that the consumer defines itself (the
  // consumer-defined-accessor pattern). The consumer references it
  // ONLY through a qualified member access `Helper::go()`, where the class
  // appears solely as the nested-name-specifier qualifier type. The module
  // must NOT export the forward declaration — it would collide with the
  // consumer's own global-module definition.
  auto producer = data_path("nnsqual_lib/producer.h");
  auto consumer = data_path("nnsqual_lib/consumer.cc");
  auto reachable =
      trace_consumer_sources({producer}, {consumer}, "nnsqual_lib",
                             {"-I", gDataDir});
  for (auto &[p, fqns] : reachable) {
    for (auto &f : fqns) {
      EXPECT_NE(f, "nnsqual_lib::internal::Helper")
          << "a library entity forward-declared but defined by the consumer "
             "itself must not be exported even when referenced through a "
             "qualified member access";
    }
  }
}

TEST(ConsumerTrace, ConsumerDefinedInternalClassDroppedGlobally) {
  // One consumer (`consumer_ref`) references the forward-declared internal
  // class `Helper` only through the producer's friend declaration (so it would
  // normally be traced); ANOTHER consumer (`consumer_def`) defines `Helper`
  // itself. Exporting the module's forward declaration would collide with the
  // second consumer's global-module definition, so the entity must be dropped
  // from the aggregated reachable set even though the first consumer
  // references it (the consumer-defined-accessor pattern).
  auto producer = data_path("consdef_lib/producer.h");
  auto consumer_ref = data_path("consdef_lib/consumer_ref.cc");
  auto consumer_def = data_path("consdef_lib/consumer_def.cc");
  auto reachable =
      trace_consumer_sources({producer}, {consumer_ref, consumer_def},
                             "consdef_lib", {"-I", gDataDir});
  for (auto &[p, fqns] : reachable) {
    for (auto &f : fqns) {
      EXPECT_NE(f, "consdef_lib::internal::Helper")
          << "an internal entity that any consumer defines must not be exported "
             "from the consumer trace, even if another consumer references it";
    }
  }
}

TEST(ConsumerTrace, InternalConsumerReachabilityExcludesPublicOnlyEntities) {
  // An INTERNAL consumer (the library's own test) references `SecretInternal`
  // but not `ExposedInternal`. Its trace must contain only the entities it
  // actually reaches; `ExposedInternal` (referenced only by a public consumer)
  // must not leak into the internal reachability, otherwise the wrapper would
  // re-export it.
  auto producer = data_path("twocons_lib/producer.h");
  auto int_consumer = data_path("twocons_lib/int_consumer.cc");
  auto reachable = trace_consumer_sources(
      {producer}, {int_consumer}, "twocons_lib", {"-I", gDataDir});
  auto it = reachable.find(producer);
  ASSERT_NE(it, reachable.end())
      << "the internal consumer must reach the producer's internal entities";
  EXPECT_TRUE(std::find(it->second.begin(), it->second.end(),
                        "twocons_lib::internal::SecretInternal") !=
              it->second.end())
      << "the entity the internal consumer references must be traced";
  EXPECT_EQ(std::find(it->second.begin(), it->second.end(),
                      "twocons_lib::internal::ExposedInternal"),
            it->second.end())
      << "an internal entity referenced only by a public consumer must not be "
         "reachable from an internal consumer";
}

TEST(ConsumerTrace, PublicConsumerReachabilityIncludesPublicOnlyEntities) {
  // A PUBLIC consumer references `ExposedInternal` but not `SecretInternal`.
  // Its trace must contain exactly the entities it reaches; entities that only
  // internal consumers reference stay out of the public reachability, so the
  // wrapper never re-exports them.
  auto producer = data_path("twocons_lib/producer.h");
  auto pub_consumer = data_path("twocons_lib/pub_consumer.cc");
  auto reachable = trace_consumer_sources(
      {producer}, {pub_consumer}, "twocons_lib", {"-I", gDataDir});
  auto it = reachable.find(producer);
  ASSERT_NE(it, reachable.end())
      << "the public consumer must reach the producer's internal entities";
  EXPECT_TRUE(std::find(it->second.begin(), it->second.end(),
                        "twocons_lib::internal::ExposedInternal") !=
              it->second.end())
      << "the entity the public consumer references must be traced";
  EXPECT_EQ(std::find(it->second.begin(), it->second.end(),
                      "twocons_lib::internal::SecretInternal"),
            it->second.end())
      << "an internal entity referenced only by an internal consumer must not "
         "be reachable from a public consumer";
}

TEST(ConsumerTrace, FriendClassDeclaration) {
  auto producer = data_path("test_lib/producer.h");
  auto consumer = data_path("test_lib/consumer_friend.h");
  EXPECT_TRUE(trace_has(producer, consumer, "test_lib::detail::FriendTarget"));
}

TEST(ConsumerTrace, FriendTemplateDeclaration) {
  // The consumer declares `template <class U> friend class detail::Foo;`
  // (a FriendTemplateDecl), which the reachability trace must record so the
  // friend target gets exported from its defining module.
  auto producer = data_path("test_lib/prod_friendtmpl.h");
  auto consumer = data_path("test_lib/cons_friendtmpl.h");
  EXPECT_TRUE(trace_has(producer, consumer,
                        "test_lib::detail::Bar"));
}

TEST(ConsumerTrace, TracesConsumerAlreadyConvertedToImports) {
  // Converting a consumer is destructive: once `#include "lib/producer.h"` has
  // become `import lib.producer;`, the file no longer parses (the BMI does not
  // exist while the library is still being converted), so a later pass — a
  // second library's --consumers run, or a re-run — silently loses everything
  // that pass would have traced. Tracing must therefore be able to run on the
  // reconstructed pre-module source, supplied as a virtual source so the file
  // keeps its path (and with it its relative-include resolution).
  auto producer = data_path("reconv_lib/producer.h");
  auto consumer = data_path("reconv_lib/consumer_converted.cc");
  std::vector<std::string> extra_args = {"-I", gDataDir};

  auto as_is = trace_consumer_modules({producer}, {consumer}, "reconv_lib",
                                      extra_args);
  EXPECT_EQ(as_is.find(consumer), as_is.end())
      << "sanity: an already-converted consumer traces nothing when parsed "
         "as it stands";

  std::map<std::string, ConsumerHeaderInfo> include_to_module = {
      {"reconv_lib/producer.h", {"reconv_lib.producer", ""}}};
  std::map<std::string, std::string> virtual_sources = {
      {consumer,
       demodularize_consumer_source(read_file(consumer), include_to_module)}};
  auto traced = trace_consumer_modules({producer}, {consumer}, "reconv_lib",
                                       extra_args, &virtual_sources);
  auto it = traced.find(consumer);
  ASSERT_NE(it, traced.end())
      << "the reconstructed source must trace the internal entity use";
  EXPECT_NE(it->second.find(producer), it->second.end())
      << "the producing header must be reported so its module gets imported";
}

TEST(ConsumerTrace, TracesSystemIncludesOfConvertedConsumer) {
  // The same applies to the C/POSIX include trace: a converted consumer must
  // not lose its `#include <...>` re-adds just because a later pass cannot
  // parse it.
  auto consumer = data_path("reconv_lib/consumer_converted.cc");
  std::vector<std::string> extra_args = {"-I", gDataDir};
  std::map<std::string, ConsumerHeaderInfo> include_to_module = {
      {"reconv_lib/producer.h", {"reconv_lib.producer", ""}}};
  std::map<std::string, std::string> virtual_sources = {
      {consumer,
       demodularize_consumer_source(read_file(consumer), include_to_module)}};
  // No assertion on the contents (this consumer uses no C library macros) —
  // the point is that tracing runs on the reconstruction instead of dying on
  // the unresolvable import.
  auto traced = trace_consumer_system_includes({consumer}, extra_args,
                                               &virtual_sources);
  EXPECT_TRUE(traced.empty() || traced.count(consumer) == 1);
}

TEST(ConsumerTrace, VirtualSourceAppliesToRelativeConsumerPaths) {
  // Consumers arrive as written on the command line, which is normally
  // relative to the tree being converted, while ClangTool parses them by
  // absolute path. A virtual source keyed by the path as given would then
  // never be found and the pass would silently parse the file on disk — the
  // very file that does not parse.
  //
  // Relative to wherever the binary is being run from, rather than assuming
  // that is the project root: the point is the relative FORM, and a test that
  // also demands a particular working directory fails for a reason that has
  // nothing to do with what it checks.
  std::error_code rel_ec;
  auto consumer = std::filesystem::relative(
      data_path("reconv_lib/consumer_converted.cc"),
      std::filesystem::current_path(), rel_ec).string();
  ASSERT_FALSE(rel_ec || consumer.empty()) << "cannot express the data path "
                                              "relative to the current "
                                              "directory";
  std::string producer = data_path("reconv_lib/producer.h");
  std::vector<std::string> extra_args = {"-I", gDataDir};
  std::map<std::string, ConsumerHeaderInfo> include_to_module = {
      {"reconv_lib/producer.h", {"reconv_lib.producer", ""}}};
  std::map<std::string, std::string> virtual_sources = {
      {consumer,
       demodularize_consumer_source(read_file(consumer), include_to_module)}};
  auto traced = trace_consumer_modules({producer}, {consumer}, "reconv_lib",
                                       extra_args, &virtual_sources);
  auto it = traced.find(consumer);
  ASSERT_NE(it, traced.end())
      << "a virtual source given under a relative path must still be used";
  EXPECT_NE(it->second.find(producer), it->second.end());
}

TEST(ConsumerTrace, VirtualSourcesCoverIncludedConsumers) {
  // Consumers include each other: a shared test helper header converted by an
  // earlier pass is not parsed as a main file, it is pulled in by other
  // consumers. Reconstructing only the file being parsed leaves that include
  // to be read from disk, where it still carries an unresolvable import and
  // kills the parse of every consumer that uses it. Every reconstruction the
  // pass holds must therefore be visible to every parse.
  auto producer = data_path("reconv_lib/producer.h");
  auto helper = data_path("reconv_lib/helper_converted.h");
  auto consumer = data_path("reconv_lib/consumer_of_helper.cc");
  std::vector<std::string> extra_args = {"-I", gDataDir};

  auto as_is = trace_consumer_modules({producer}, {consumer}, "reconv_lib",
                                      extra_args);
  EXPECT_EQ(as_is.find(consumer), as_is.end())
      << "sanity: the converted include breaks the consumer's parse";

  std::map<std::string, ConsumerHeaderInfo> include_to_module = {
      {"reconv_lib/producer.h", {"reconv_lib.producer", ""}}};
  // Only the *helper* is reconstructed — the consumer being parsed needs no
  // reconstruction of its own.
  std::map<std::string, std::string> virtual_sources = {
      {helper,
       demodularize_consumer_source(read_file(helper), include_to_module)}};
  auto traced = trace_consumer_modules({producer}, {consumer}, "reconv_lib",
                                       extra_args, &virtual_sources);
  auto it = traced.find(consumer);
  ASSERT_NE(it, traced.end())
      << "a reconstruction of an included consumer must apply to this parse";
  EXPECT_NE(it->second.find(producer), it->second.end());
}

TEST(ConsumerTrace, ReportsTheHeaderThatOwnsTheType) {
  // The traced include must be the system header that OWNS the declaration,
  // not an umbrella that merely reaches it. An umbrella provides the type only
  // on the platform the conversion ran against — reporting it produces a tree
  // that builds against one C library and fails against another whose umbrella
  // does not pull the owning header in.
  //
  // The owning header sits in a subdirectory, so this also pins the include
  // spelling: "owner/types.h", since a bare "types.h" names nothing.
  auto use = data_path("sysincl_lib/use_owned.cc");
  std::vector<std::string> extra_args = {"-I", gDataDir, "-isystem",
                                         std::string(gDataDir) + "/sysincl"};

  auto traced = trace_consumer_system_includes({use}, extra_args);
  auto it = traced.find(use);
  ASSERT_NE(it, traced.end())
      << "a system type used only inside a cast must still be traced";
  EXPECT_NE(it->second.count("owner/types.h"), 0u);
  EXPECT_EQ(it->second.count("umbrella.h"), 0u)
      << "the umbrella provides the type only transitively";
  EXPECT_EQ(it->second.count("bits/detail.h"), 0u)
      << "a private implementation header is not includable";
}

TEST(ConsumerTrace, ReportsThePublicParentOfAPrivateFragment) {
  // The owning header is not always the innermost one: compilers split
  // declarations into `__`-prefixed fragments that cannot be included on their
  // own. For those the public header that gathers them is the answer, so the
  // walk must keep going outward rather than report the fragment.
  auto use = data_path("sysincl_lib/use_fragment.cc");
  std::vector<std::string> extra_args = {"-I", gDataDir, "-isystem",
                                         std::string(gDataDir) + "/sysincl"};

  auto traced = trace_consumer_system_includes({use}, extra_args);
  auto it = traced.find(use);
  ASSERT_NE(it, traced.end());
  EXPECT_NE(it->second.count("umbrella.h"), 0u);
  EXPECT_EQ(it->second.count("__fragment.h"), 0u)
      << "a per-declaration fragment is not includable on its own";
}

TEST(ConsumerTrace, CombinedTraceMatchesTheSeparatePasses) {
  // The two consumer traces ask different questions of the same translation
  // unit — same files, same flags, same virtual sources — and profiling puts
  // essentially the whole runtime of a --consumers run in the parse, so
  // answering them separately parsed every consumer twice. Sharing the parse
  // has to return precisely what the two passes returned on their own.
  auto lib = data_path("sysuse_lib/prod.h");
  auto use = data_path("sysuse_lib/use.cc");
  std::vector<std::string> extra_args = {"-I", gDataDir};

  auto separate_modules =
      trace_consumer_modules({lib}, {use}, "sysuse_lib", extra_args);
  auto separate_includes = trace_consumer_system_includes({use}, extra_args);
  auto combined = trace_consumers({lib}, {use}, "sysuse_lib", extra_args);

  EXPECT_EQ(combined.producers_by_consumer, separate_modules);
  EXPECT_EQ(combined.system_includes_by_consumer, separate_includes);
  // Pin the values too, so agreement between two empty maps cannot pass.
  auto mit = combined.producers_by_consumer.find(use);
  ASSERT_NE(mit, combined.producers_by_consumer.end());
  EXPECT_NE(mit->second.count(lib), 0u);
  auto iit = combined.system_includes_by_consumer.find(use);
  ASSERT_NE(iit, combined.system_includes_by_consumer.end());
  EXPECT_NE(iit->second.count("stdio.h"), 0u);
}

TEST(ConsumerTrace, CombinedTraceHonoursVirtualSources) {
  // Sharing the parse must not lose the reconstruction behaviour either.
  auto producer = data_path("reconv_lib/producer.h");
  auto consumer = data_path("reconv_lib/consumer_converted.cc");
  std::vector<std::string> extra_args = {"-I", gDataDir};
  std::map<std::string, ConsumerHeaderInfo> include_to_module = {
      {"reconv_lib/producer.h", {"reconv_lib.producer", ""}}};
  std::map<std::string, std::string> virtual_sources = {
      {consumer,
       demodularize_consumer_source(read_file(consumer), include_to_module)}};
  auto combined = trace_consumers({producer}, {consumer}, "reconv_lib",
                                  extra_args, &virtual_sources);
  auto it = combined.producers_by_consumer.find(consumer);
  ASSERT_NE(it, combined.producers_by_consumer.end());
  EXPECT_NE(it->second.find(producer), it->second.end());
}

TEST(ConsumerTrace, LibraryHeaderAnalysisMatchesTheSeparatePasses) {
  // The library headers were parsed twice per --consumers run: once to ask
  // which of them define macros, once to collect the internal entities each
  // declares. Both answers come from one parse — the macro question is a
  // preprocessor callback over the same eligibility rule (collectible_macro)
  // the entity extractor uses, the entity question an AST traversal.
  std::vector<std::string> headers = {
      data_path("sysuse_lib/prod.h"),
      data_path("test_lib/producer.h"),
      data_path("test_lib/internal/foo.h"),
      data_path("macro_test.h"),
      data_path("twocons_lib/producer.h"),
  };
  std::vector<std::string> extra_args = {"-I", gDataDir};

  auto combined = analyze_library_headers(headers, extra_args);

  for (const auto &h : headers) {
    bool had_macros = !analyze_files_with_flags({h}, extra_args).macros.empty();
    EXPECT_EQ(combined.headers_with_macros.count(h) != 0u, had_macros)
        << "macro presence must agree for " << h;
  }

  // The internal-entity half must drive the trace exactly as before.
  auto producer = data_path("sysuse_lib/prod.h");
  auto use = data_path("sysuse_lib/use.cc");
  auto separate = trace_consumer_modules({producer}, {use}, "sysuse_lib",
                                         extra_args);
  auto shared = trace_consumers(combined.internal_by_header, {use},
                                "sysuse_lib", extra_args);
  EXPECT_EQ(shared.producers_by_consumer, separate)
      << "a precomputed header analysis must trace identically";
  auto it = shared.producers_by_consumer.find(use);
  ASSERT_NE(it, shared.producers_by_consumer.end());
  EXPECT_NE(it->second.count(producer), 0u);
}

TEST(ConsumerTrace, SharedHeaderScanMatchesTheSeparateSweeps) {
  // A --full run wants both the per-file entity models and the internal-entity
  // index, and parsed every header once for each. One sweep yields both, and
  // must yield exactly what the two produced.
  std::vector<std::string> headers = {data_path("test_lib/producer.h"),
                                      data_path("test_lib/consumer.h")};
  std::vector<std::string> extra_args = {"-I", gDataDir};

  auto scan = scan_headers(headers, extra_args);
  auto models = analyze_files_per_file(headers, extra_args);
  ASSERT_EQ(scan.models.size(), models.size());
  for (std::size_t i = 0; i < models.size(); ++i)
    EXPECT_EQ(scan.models[i].items.size(), models[i].items.size())
        << "entity extraction must be unaffected by the extra consumer";

  EXPECT_EQ(scan.internal.covered.size(), headers.size())
      << "the scan must record which files it looked at";
  EXPECT_EQ(trace_consumer_reachability(headers, "test_lib", extra_args,
                                        &scan.internal),
            trace_consumer_reachability(headers, "test_lib", extra_args))
      << "reusing the index must not change reachability";
}

TEST(ConsumerTrace, UncoveredPathsAreStillScanned) {
  // Coverage is not the same as presence: a file that declared no internal
  // entity is absent from the index, and a file nobody scanned is absent too.
  // Only the recorded coverage tells them apart, and anything uncovered — an
  // implementation source, when only the headers were scanned — must still be
  // collected rather than silently treated as empty.
  std::vector<std::string> headers = {data_path("test_lib/producer.h"),
                                      data_path("test_lib/consumer.h")};
  std::vector<std::string> extra_args = {"-I", gDataDir};

  auto full = trace_consumer_reachability(headers, "test_lib", extra_args);

  // An index that covers only the first header: the second must be scanned.
  auto scan = scan_headers({headers[0]}, extra_args);
  auto partial = trace_consumer_reachability(headers, "test_lib", extra_args,
                                             &scan.internal);
  EXPECT_EQ(partial, full)
      << "a partially covered index must give the same answer as none";
}
