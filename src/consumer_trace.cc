module;

export module modulizer.consumer_trace;
import modulizer.analyzer;
import modulizer.astutil;
import modulizer.include_analysis;
import modulizer.trace_visitors;
import modulizer.util;
import libtooling;
import std;

std::map<std::string, std::vector<std::string>> build_header_deps(
    const std::vector<std::string> &header_paths,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args) {
  std::map<std::string, std::vector<std::string>> deps;

  for (auto &consumer : header_paths) {
    auto src = read_file(consumer);
    if (src.empty()) continue;
    auto includes = parse_includes(src);
    annotate_guards(src, includes);

    auto lib_prefix = std::format("{}/", library_name.str());
    for (auto &inc : includes) {
      if (!inc.is_quoted) continue;
      if (is_xmacro_include(inc.path)) continue;
      if (!inc.path.starts_with(lib_prefix)) continue;
      if (inc.path.contains("/custom/")) continue;

      auto resolved = resolve_include(inc.path, consumer, extra_args);
      if (!resolved.empty())
        deps[consumer].push_back(resolved);
    }
  }
  return deps;
}

// Collect the internal-namespace entity FQNs of each library header (shared by
// the three trace functions below). `verbose` prints a per-header count line.
// Records whether the main file defines any macro a generated macro header
// would carry. Same eligibility rule as the entity extractor's collector, so
// the two agree by construction rather than by coincidence.
class MacroPresence : public clang::PPCallbacks {
public:
  MacroPresence(const clang::SourceManager &sm, char &out) : sm(sm), out(out) {}

  void MacroDefined(const clang::Token &,
                    const clang::MacroDirective *md) override {
    if (collectible_macro(md, sm)) out = 1;
  }

private:
  const clang::SourceManager &sm;
  char &out;
};

// Collects, per library header, the internal entities it declares — and, when
// `headers_with_macros` is given, whether it defines any macro. Both answers
// come from the one parse: the run used to parse every library header twice to
// ask them separately.
std::map<std::string, std::set<std::string>> collect_internal_by_header(
    const std::vector<std::string> &header_paths,
    const std::vector<std::string> &extra_args, bool verbose,
    std::set<std::string> *headers_with_macros = nullptr) {
  std::map<std::string, std::set<std::string>> internal_by_header;
  std::vector<std::set<std::string>> collected(header_paths.size());
  // Not vector<bool>: the workers write to distinct elements concurrently.
  std::vector<char> defines_macros(header_paths.size(), 0);
  const bool want_macros = headers_with_macros != nullptr;
  parallel_parse(
      header_paths, extra_args, /*delayed_template_parsing=*/true,
      [&](std::size_t i, const std::string &) {
        auto &entities = collected[i];
        char &macros = defines_macros[i];
        return std::make_unique<VisitorFrontendActionFactory>(
            [&entities, &macros, want_macros](clang::CompilerInstance &ci) {
              if (want_macros)
                ci.getPreprocessor().addPPCallbacks(
                    std::make_unique<MacroPresence>(ci.getSourceManager(),
                                                    macros));
              return make_traverse_consumer<InternalCollector>(&entities);
            });
      });
  if (want_macros)
    for (std::size_t i = 0; i < header_paths.size(); ++i)
      if (defines_macros[i]) headers_with_macros->insert(header_paths[i]);
  for (std::size_t i = 0; i < header_paths.size(); ++i) {
    if (verbose)
      llvm::outs() << "  " << std::filesystem::path(header_paths[i])
                                  .filename().string()
                   << ": " << collected[i].size() << " internal entities\n";
    if (!collected[i].empty())
      internal_by_header[header_paths[i]] = std::move(collected[i]);
  }
  return internal_by_header;
}

// One pass of the transitive alias closure (shared by the trace functions): an
// internal alias whose underlying type references a reachable entity is itself
// needed by the consumers, but dependent-member references
// (`typename Alias<...>::type`) resolve to the underlying template in the AST,
// so the alias name never reaches the trace. Returns true when any new entity
// was added (callers iterate to a fixed point).
bool run_alias_closure(
    const std::map<std::string, std::set<std::string>> &internal_by_header,
    std::map<std::string, std::set<std::string>> &reachable,
    const std::vector<std::string> &extra_args) {
  bool changed = false;
  std::vector<std::string> producers;
  for (auto &[p, f] : reachable)
    if (internal_by_header.count(p)) producers.push_back(p);
  std::vector<std::map<std::string, std::set<std::string>>> partials(
      producers.size());
  parallel_parse(
      producers, extra_args, /*delayed_template_parsing=*/true,
      [&](std::size_t i, const std::string &producer) {
        const auto &internal = internal_by_header.find(producer)->second;
        const auto &fqns = reachable.find(producer)->second;
        std::set<std::string> &out = partials[i][producer];
        return std::make_unique<VisitorFrontendActionFactory>(
            [&internal, &fqns, &out](clang::CompilerInstance &)
                -> std::unique_ptr<clang::ASTConsumer> {
              // A producer's closure only ever grows its own set, so its fixed
              // point is reachable without leaving this parse: traverse again
              // with what the last round found until nothing new appears.
              // Re-parsing the file per round is what this used to cost.
              return std::make_unique<TraverseConsumer>(
                  [&internal, &fqns, &out](clang::ASTContext &ctx) {
                    std::set<std::string> acc(fqns.begin(), fqns.end());
                    for (;;) {
                      std::set<std::string> round;
                      AliasReachabilityCloser v(ctx.getSourceManager(),
                                                internal, acc, round);
                      v.TraverseDecl(ctx.getTranslationUnitDecl());
                      bool grew = false;
                      for (auto &f : round)
                        if (acc.insert(f).second) grew = true;
                      if (!grew) break;
                    }
                    for (auto &f : acc)
                      if (!fqns.count(f)) out.insert(f);
                  });
            });
      });
  for (auto &partial : partials)
    for (auto &[producer, out] : partial)
      for (auto &f : out)
        if (reachable[producer].insert(f).second) changed = true;
  return changed;
}

// Convert the set-valued reachability map to the returned vector form.
std::map<std::string, std::vector<std::string>> to_vector_map(
    const std::map<std::string, std::set<std::string>> &reachable) {
  std::map<std::string, std::vector<std::string>> result;
  for (auto &[producer, fqns] : reachable)
    result[producer] = std::ranges::to<std::vector>(fqns);
  return result;
}

// The internal entities declared per file, together with the set of files
// actually scanned. Coverage has to be explicit: a file absent from the map
// declared none, which is not the same as a file nobody looked at.
export struct InternalEntityIndex {
  std::map<std::string, std::set<std::string>> by_header;
  std::set<std::string> covered;
};

// One sweep of `paths` producing both the per-file entity models and the
// internal-entity index. A --full run wants both, and used to parse every
// header once for each.
export struct HeaderScan {
  std::vector<EntityModel> models;
  InternalEntityIndex internal;
};

export HeaderScan scan_headers(const std::vector<std::string> &paths,
                               const std::vector<std::string> &extra_args) {
  HeaderScan scan;
  scan.models.resize(paths.size());
  std::vector<std::vector<std::string>> ifdef_macros(paths.size());
  std::vector<std::set<std::string>> internal(paths.size());
  parallel_parse(paths, extra_args, /*delayed_template_parsing=*/false,
                 [&](std::size_t i, const std::string &) {
                   auto &entities = internal[i];
                   return std::make_unique<EntityExtractionFactory>(
                       scan.models[i], ifdef_macros[i], /*wrapper_mode=*/false,
                       /*undefed=*/nullptr,
                       [&entities](clang::CompilerInstance &) {
                         return make_traverse_consumer<InternalCollector>(
                             &entities);
                       });
                 });
  for (std::size_t i = 0; i < paths.size(); ++i) {
    scan.internal.covered.insert(paths[i]);
    if (!internal[i].empty())
      scan.internal.by_header[paths[i]] = std::move(internal[i]);
  }
  return scan;
}

export std::map<std::string, std::vector<std::string>>
trace_consumer_reachability(
    const std::vector<std::string> &header_paths,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args,
    const InternalEntityIndex *precomputed = nullptr,
    const std::function<std::unique_ptr<clang::ASTConsumer>(
        clang::CompilerInstance &, const std::string &)> &extra = {}) {

  // Whatever a caller already scanned is reused; anything it did not cover —
  // implementation sources, say, when only the headers were scanned — is
  // collected here.
  std::map<std::string, std::set<std::string>> internal_by_header;
  std::vector<std::string> to_scan;
  if (precomputed) {
    internal_by_header = precomputed->by_header;
    for (const auto &p : header_paths)
      if (!precomputed->covered.count(p)) to_scan.push_back(p);
  } else {
    to_scan = header_paths;
  }
  if (!to_scan.empty()) {
    auto rest = collect_internal_by_header(to_scan, extra_args,
                                           /*verbose=*/true);
    for (auto &[path, fqns] : rest) internal_by_header[path] = std::move(fqns);
  }

  auto deps = build_header_deps(header_paths, library_name, extra_args);
  llvm::outs() << "  deps: " << deps.size() << " consumers\n";

  // Compute transitive closure: for each consumer, find all producers
  // reachable via direct or indirect includes.
  std::map<std::string, std::set<std::string>> transitive_deps;
  for (auto &consumer : header_paths) {
    std::set<std::string> seen;
    std::vector<std::string> stack = {consumer};
    while (!stack.empty()) {
      auto cur = stack.back();
      stack.pop_back();
      auto it = deps.find(cur);
      if (it == deps.end()) continue;
      for (auto &prod : it->second) {
        if (seen.insert(prod).second)
          stack.push_back(prod);
      }
    }
    if (!seen.empty())
      transitive_deps[consumer] = std::move(seen);
  }

  std::map<std::string, std::set<std::string>> reachable;
  {
    std::vector<std::string> consumers;
    for (auto &[c, p] : transitive_deps) consumers.push_back(c);
    std::vector<std::map<std::string, std::set<std::string>>> partials(
        consumers.size());
    parallel_parse(
        consumers, extra_args, /*delayed_template_parsing=*/true,
        [&](std::size_t i, const std::string &path) {
          auto &local = partials[i];
          return std::make_unique<VisitorFrontendActionFactory>(
              [&internal_by_header, &local, &extra,
               self = path](clang::CompilerInstance &ci) {
                auto own = std::make_unique<ConsumerRefConsumer>(
                    ci.getSourceManager(), internal_by_header, local, nullptr);
                // Another analysis of the same file rides this parse. It
                // decides per path whether it wants this one at all, so a
                // header-only analysis is not fed implementation sources.
                std::unique_ptr<clang::ASTConsumer> rider;
                if (extra) rider = extra(ci, self);
                if (!rider) return std::unique_ptr<clang::ASTConsumer>(std::move(own));
                std::vector<std::unique_ptr<clang::ASTConsumer>> both;
                both.push_back(std::move(own));
                both.push_back(std::move(rider));
                return make_combined_consumer(std::move(both));
              });
        });
    for (auto &partial : partials)
      for (auto &[producer, fqns] : partial)
        reachable[producer].insert_range(fqns);
  }

  // Transitive closure over aliases: an internal alias whose underlying type
  // references a reachable entity is itself needed by the consumers, but
  // dependent-member references (`typename Alias<...>::type`) resolve to the
  // underlying template in the AST, so the alias name never reaches the trace.
  // One call suffices: each producer's closure converges inside its own parse.
  run_alias_closure(internal_by_header, reachable, extra_args);

  auto result = to_vector_map(reachable);

  llvm::outs() << "Auto-traced " << result.size()
                << " modules with consumer-reachable internal entities\n";
  return result;
}

// Like trace_consumer_reachability, but the consumers are EXPLICIT source files
// (e.g. a library's own test/main sources) passed on the command line rather
// than the headers being rewritten. Each consumer is analyzed for references to
// internal entities of the library headers; those entities must be exported so
// the consumers can use them through the module imports. Consumers are not
// rewritten themselves.
export std::map<std::string, std::vector<std::string>>
trace_consumer_sources(
    const std::vector<std::string> &header_paths,
    const std::vector<std::string> &consumer_sources,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args) {

  std::map<std::string, std::set<std::string>> internal_by_header =
      collect_internal_by_header(header_paths, extra_args, /*verbose=*/false);

  std::map<std::string, std::set<std::string>> reachable;
  {
    std::vector<std::map<std::string, std::set<std::string>>> partials(
        consumer_sources.size());
    std::vector<std::set<std::string>> consumer_defined(consumer_sources.size());
    parallel_parse(
        consumer_sources, extra_args, /*delayed_template_parsing=*/true,
        [&](std::size_t i, const std::string &) {
          auto &local = partials[i];
          auto &defined = consumer_defined[i];
          return std::make_unique<VisitorFrontendActionFactory>(
              [&internal_by_header, &local, &defined](
                  clang::CompilerInstance &ci) {
                return std::make_unique<ConsumerRefConsumer>(
                    ci.getSourceManager(), internal_by_header, local, &defined);
              });
        },
        [&](std::size_t i, const std::string &consumer) {
          llvm::outs()
              << "  traced consumer "
              << std::filesystem::path(consumer).filename().string() << "\n";
        });
    // GLOBAL filter: if ANY consumer defines an internal entity in its own
    // main file (the library only forward-declares it), do not export it from
    // any consumer's trace — the module's forward declaration would collide
    // with that consumer's global-module definition.
    std::set<std::string> any_consumer_defined;
    for (auto &defined : consumer_defined)
      any_consumer_defined.insert_range(defined);
    for (auto &partial : partials)
      for (auto &[producer, fqns] : partial)
        for (auto &f : fqns)
          if (!any_consumer_defined.count(f))
            reachable[producer].insert(f);
  }

  // Transitive closure over aliases (same as trace_consumer_reachability).
  // One call suffices: each producer's closure converges inside its own parse.
  run_alias_closure(internal_by_header, reachable, extra_args);

  auto result = to_vector_map(reachable);

  llvm::outs() << "Traced " << result.size()
                << " modules with consumer-source reachable internal entities\n";
  return result;
}

// Per-consumer variant: for every consumer source, the set of library headers
// whose INTERNAL entities the consumer references (directly or through macro
// expansions). Those entities are exported from their defining sub-modules but
// not re-exported by the library wrapper (internal-consumer reachability is not
// public API), so each consumer must import the owning modules itself. Unlike
// trace_consumer_sources this keeps the per-consumer attribution (no merge), so
// a consumer that never touches a sub-module's internals does not import it.
// `virtual_sources`, when given, supplies the content to parse for a consumer
// instead of its on-disk form — used to trace consumers an earlier pass already
// converted to imports (see demodularize_consumer_source), which no longer
// parse as they stand.
// Both consumer traces answered in one parse per consumer. They ask different
// questions of the same translation unit — same files, same flags, same
// virtual sources — and profiling puts essentially the whole runtime in the
// parse, so answering them separately parsed every consumer twice for nothing.
export struct ConsumerTrace {
  // Consumer path -> paths of the library headers whose internal entities it
  // references (whose modules it must therefore import).
  std::map<std::string, std::set<std::string>> producers_by_consumer;
  // Consumer path -> system C/POSIX include names it must get back.
  std::map<std::string, std::set<std::string>> system_includes_by_consumer;
};

// Everything a --consumers run needs from the library headers, from one parse
// of each: which internal entities each declares, and which of them define
// macros a consumer must include the generated macro header for.
export struct LibraryHeaderAnalysis {
  std::map<std::string, std::set<std::string>> internal_by_header;
  std::set<std::string> headers_with_macros;
};

export LibraryHeaderAnalysis analyze_library_headers(
    const std::vector<std::string> &header_paths,
    const std::vector<std::string> &extra_args, bool verbose = false) {
  LibraryHeaderAnalysis out;
  out.internal_by_header = collect_internal_by_header(
      header_paths, extra_args, verbose, &out.headers_with_macros);
  return out;
}

// Trace against an already-computed header analysis, so a caller that needed
// it for its own reasons does not pay for a second sweep of the headers.
export ConsumerTrace trace_consumers(
    const std::map<std::string, std::set<std::string>> &internal_by_header,
    const std::vector<std::string> &consumer_sources,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args,
    const std::map<std::string, std::string> *virtual_sources = nullptr);

export ConsumerTrace trace_consumers(
    const std::vector<std::string> &header_paths,
    const std::vector<std::string> &consumer_sources,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args,
    const std::map<std::string, std::string> *virtual_sources = nullptr) {

  return trace_consumers(
      collect_internal_by_header(header_paths, extra_args, /*verbose=*/false),
      consumer_sources, library_name, extra_args, virtual_sources);
}

ConsumerTrace trace_consumers(
    const std::map<std::string, std::set<std::string>> &internal_by_header,
    const std::vector<std::string> &consumer_sources,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args,
    const std::map<std::string, std::string> *virtual_sources) {
  std::vector<std::map<std::string, std::set<std::string>>> partials(
      consumer_sources.size());
  std::vector<std::set<std::string>> consumer_defined(consumer_sources.size());
  std::vector<std::set<std::string>> system_includes(consumer_sources.size());
  parallel_parse(
      consumer_sources, extra_args,
      ParseOptions{/*delayed_template_parsing=*/true, virtual_sources},
      [&](std::size_t i, const std::string &) {
        auto &local = partials[i];
        auto &defined = consumer_defined[i];
        auto &sys = system_includes[i];
        return std::make_unique<VisitorFrontendActionFactory>(
            [&internal_by_header, &local, &defined,
             &sys](clang::CompilerInstance &ci) {
              // The macro-expansion half is a preprocessor callback, so it has
              // to ride on this same parse — which is why the two analyses
              // share a parse rather than a cached AST.
              ci.getPreprocessor().addPPCallbacks(
                  std::make_unique<SystemHeaderUsageTracer>(ci, sys));
              std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
              consumers.push_back(std::make_unique<ConsumerRefConsumer>(
                  ci.getSourceManager(), internal_by_header, local, &defined));
              consumers.push_back(
                  make_traverse_consumer<SystemHeaderRefFinder>(sys));
              return make_combined_consumer(std::move(consumers));
            });
      });

  // GLOBAL filter: if ANY consumer defines an internal entity in its own main
  // file, importing the module's forward declaration is unnecessary (and could
  // collide), so drop the reference from every consumer's attribution.
  std::set<std::string> any_consumer_defined;
  for (auto &defined : consumer_defined)
    any_consumer_defined.insert_range(defined);

  ConsumerTrace result;
  for (std::size_t i = 0; i < consumer_sources.size(); ++i) {
    std::set<std::string> mods;
    for (auto &[producer, fqns] : partials[i]) {
      bool any = false;
      for (auto &f : fqns)
        if (!any_consumer_defined.count(f)) {
          any = true;
          break;
        }
      if (any) mods.insert(producer);
    }
    if (!mods.empty())
      result.producers_by_consumer[consumer_sources[i]] = std::move(mods);
    if (!system_includes[i].empty())
      result.system_includes_by_consumer[consumer_sources[i]] =
          std::move(system_includes[i]);
  }
  return result;
}

// The producer half on its own, for callers that want only that question.
export std::map<std::string, std::set<std::string>> trace_consumer_modules(
    const std::vector<std::string> &header_paths,
    const std::vector<std::string> &consumer_sources,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args,
    const std::map<std::string, std::string> *virtual_sources = nullptr) {
  return trace_consumers(header_paths, consumer_sources, library_name,
                         extra_args, virtual_sources)
      .producers_by_consumer;
}

// For every consumer source, the set of system C/POSIX include names (e.g.
// "errno.h", "assert.h", "pthread.h") the consumer needs re-added once the
// library headers no longer pull them in textually. `import std.compat` cannot
// provide the C library's macros (errno, assert, INT_MAX, NULL, stderr) or
// POSIX declarations/types (ssize_t, pthread_*); the consumer must include the
// defining headers itself. Macro expansions and system-header declaration
// references are traced per consumer (SystemHeaderUsageTracer /
// SystemHeaderRefFinder), so a consumer that never uses a given C header does
// not get a redundant include.
// `virtual_sources` has the same meaning as in trace_consumer_modules.
export std::map<std::string, std::set<std::string>>
trace_consumer_system_includes(
    const std::vector<std::string> &consumer_sources,
    const std::vector<std::string> &extra_args,
    const std::map<std::string, std::string> *virtual_sources = nullptr) {
  std::vector<std::set<std::string>> partials(consumer_sources.size());
  parallel_parse(
      consumer_sources, extra_args,
      ParseOptions{/*delayed_template_parsing=*/true, virtual_sources},
      [&](std::size_t i, const std::string &) {
        auto &out = partials[i];
        return std::make_unique<SystemHeaderUsageFactory>(out);
      });
  std::map<std::string, std::set<std::string>> result;
  for (std::size_t i = 0; i < consumer_sources.size(); ++i)
    if (!partials[i].empty())
      result[consumer_sources[i]] = std::move(partials[i]);
  return result;
}
