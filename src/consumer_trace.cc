module;

export module modulizer.consumer_trace;
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
std::map<std::string, std::set<std::string>> collect_internal_by_header(
    const std::vector<std::string> &header_paths,
    const std::vector<std::string> &extra_args, bool verbose) {
  std::map<std::string, std::set<std::string>> internal_by_header;
  std::vector<std::set<std::string>> collected(header_paths.size());
  parallel_parse(
      header_paths, extra_args, /*delayed_template_parsing=*/true,
      [&](std::size_t i, const std::string &) {
        auto &entities = collected[i];
        return std::make_unique<VisitorFrontendActionFactory>(
            [&entities](clang::CompilerInstance &) {
              return make_traverse_consumer<InternalCollector>(&entities);
            });
      });
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
            [&internal, &fqns, &out](clang::CompilerInstance &) {
              return make_traverse_consumer<AliasReachabilityCloser>(
                  internal, fqns, out);
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

export std::map<std::string, std::vector<std::string>>
trace_consumer_reachability(
    const std::vector<std::string> &header_paths,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args) {

  std::map<std::string, std::set<std::string>> internal_by_header =
      collect_internal_by_header(header_paths, extra_args, /*verbose=*/true);

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
        [&](std::size_t i, const std::string &) {
          auto &local = partials[i];
          return std::make_unique<VisitorFrontendActionFactory>(
              [&internal_by_header, &local](clang::CompilerInstance &ci) {
                return std::make_unique<ConsumerRefConsumer>(
                    ci.getSourceManager(), internal_by_header, local, nullptr);
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
  while (run_alias_closure(internal_by_header, reachable, extra_args)) {}

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
  while (run_alias_closure(internal_by_header, reachable, extra_args)) {}

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
export std::map<std::string, std::set<std::string>> trace_consumer_modules(
    const std::vector<std::string> &header_paths,
    const std::vector<std::string> &consumer_sources,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args) {

  std::map<std::string, std::set<std::string>> internal_by_header =
      collect_internal_by_header(header_paths, extra_args, /*verbose=*/false);

  std::vector<std::map<std::string, std::set<std::string>>> partials(
      consumer_sources.size());
  std::vector<std::set<std::string>> consumer_defined(consumer_sources.size());
  parallel_parse(
      consumer_sources, extra_args, /*delayed_template_parsing=*/true,
      [&](std::size_t i, const std::string &) {
        auto &local = partials[i];
        auto &defined = consumer_defined[i];
        return std::make_unique<VisitorFrontendActionFactory>(
            [&internal_by_header, &local, &defined](clang::CompilerInstance &ci) {
              return std::make_unique<ConsumerRefConsumer>(
                  ci.getSourceManager(), internal_by_header, local, &defined);
            });
      });

  // GLOBAL filter: if ANY consumer defines an internal entity in its own main
  // file, importing the module's forward declaration is unnecessary (and could
  // collide), so drop the reference from every consumer's attribution.
  std::set<std::string> any_consumer_defined;
  for (auto &defined : consumer_defined)
    any_consumer_defined.insert_range(defined);

  std::map<std::string, std::set<std::string>> result;
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
    if (!mods.empty()) result[consumer_sources[i]] = std::move(mods);
  }
  return result;
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
export std::map<std::string, std::set<std::string>>
trace_consumer_system_includes(const std::vector<std::string> &consumer_sources,
                               const std::vector<std::string> &extra_args) {
  std::vector<std::set<std::string>> partials(consumer_sources.size());
  parallel_parse(
      consumer_sources, extra_args, /*delayed_template_parsing=*/true,
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
