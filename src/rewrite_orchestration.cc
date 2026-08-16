module;

export module modulizer.rewrite_orchestration;
import modulizer.analyzer;
import modulizer.consumer_trace;
import modulizer.header_rewriter;
import modulizer.macro_analyzer;
import modulizer.naming;
import modulizer.rewrite_util;
import modulizer.util;
import libtooling;
import std;

// Library-level orchestration shared by the `--headers` and `--full` CLI
// entry points: reachability tracing helpers, macro-module computation, and
// the batch header-rewrite pipeline (parallel rewrite + cross-file macro-body
// export-marker routing). Keeps `modulizer.cli` a thin CLI layer.

namespace modulizer {

// Not in an anonymous namespace: referenced from exported non-inline
// functions is fine, but `filter_command_line` is deliberately file-local to
// avoid leaking it; the exported compile-args helpers below wrap it.

// Strip the source path, `-c`, and `-o [output]` from a clang command line,
// leaving the extra arguments. `skip_output_value` also drops the argument
// that follows `-o`; `prefix_match_o` drops any argument that merely starts
// with `-o`. The two call sites intentionally differ on both flags.
std::vector<std::string> filter_command_line(
    const std::vector<std::string> &cmd_line, llvm::StringRef source,
    bool skip_output_value, bool prefix_match_o) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < cmd_line.size(); ++i) {
    auto &arg = cmd_line[i];
    if (i == 0) continue;  // the compiler/tool binary name is not a flag
    if (arg == source) continue;
    if (arg == "-c") continue;
    if (arg == "-o" || (prefix_match_o && arg.starts_with("-o"))) {
      if (skip_output_value && i + 1 < cmd_line.size()) ++i;
      continue;
    }
    out.push_back(arg);
  }
  return out;
}

// Extra compile args for one source file (drops the source path, `-c`, and
// `-o <output>`).
export std::vector<std::string> compile_extra_args(
    const clang::tooling::CompilationDatabase &cdb, llvm::StringRef src) {
  std::vector<std::string> extra;
  for (auto &cmd : cdb.getCompileCommands(src))
    for (auto &a : filter_command_line(cmd.CommandLine, src,
                                       /*skip_output_value=*/true,
                                       /*prefix_match_o=*/false))
      extra.push_back(a);
  return extra;
}

// Extra args taken from only the first source path's compile commands.
// Deliberately leaks the `-o` output value (mirrors the historical behavior
// used to seed consumer/macro tracing).
export std::vector<std::string> first_extra_args(
    const clang::tooling::CompilationDatabase &cdb,
    const std::vector<std::string> &paths) {
  std::vector<std::string> extra;
  for (auto &s : paths) {
    for (auto &cmd : cdb.getCompileCommands(s))
      for (auto &a : filter_command_line(cmd.CommandLine, s,
                                         /*skip_output_value=*/false,
                                         /*prefix_match_o=*/true))
        extra.push_back(a);
    break;
  }
  return extra;
}

// Canonicalized header path, used as the routing key for cross-file macro-body
// export markers (the visitor resolves spelling locations the same way).
std::string canonical_path(llvm::StringRef p) {
  std::error_code ec;
  auto c = std::filesystem::weakly_canonical(std::filesystem::path(p.str()), ec);
  return ec ? p.str() : c.string();
}

// Output path for a rewritten header, mirroring its subdirectory structure
// under the library root. `mylib/internal/mylib-port.h` (with library name
// `mylib`) maps to `internal/mylib-port.h`, so headers that share a basename
// (e.g. mylib-port.h in different directories) never collide. Falls back to
// the flat basename when the path has no library-root segment.
std::string header_output_relpath(llvm::StringRef header_path,
                                  llvm::StringRef library_name) {
  auto segs = split_path(header_path);
  std::size_t start = segs.size();
  for (std::size_t i = 0; i < segs.size(); ++i) {
    if (segs[i] == library_name.str()) { start = i + 1; break; }
  }
  if (start == segs.size())
    return std::filesystem::path(header_path.str()).filename().string();
  std::string rel;
  for (std::size_t i = start; i < segs.size(); ++i) {
    if (!rel.empty()) rel += "/";
    rel += segs[i];
  }
  return rel;
}

// Trace a list of explicit consumer source files and merge the resulting
// producer→FQN reachability into `dest`.
export void trace_and_merge(
    std::map<std::string, std::vector<std::string>> &dest,
    const std::vector<std::string> &header_paths,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args,
    const std::vector<std::string> &consumer_sources) {
  if (consumer_sources.empty()) return;
  auto traced = trace_consumer_sources(header_paths, consumer_sources,
                                       library_name, extra_args);
  for (auto &[producer, fqns] : traced) {
    auto &d = dest[producer];
    for (auto &f : fqns) d.push_back(f);
  }
}

// The set of internal entities re-exported by the generated wrapper module:
// only PUBLIC-consumer reachable entities (plus macro-/alias-reachable ones and
// explicitly listed FQNs) are part of the facade. Entities reachable solely
// from INTERNAL consumers (the library's own tests/main/impls) are exported
// from their defining modules so those consumers work via imports, but they are
// not public API and must not be re-exported by the wrapper.
export std::vector<std::string> wrapper_reachable_fqns(
    const std::vector<std::string> &manual_fqns,
    const std::vector<std::string> &macro_reachable,
    const std::vector<std::string> &alias_reachable,
    const std::map<std::string, std::vector<std::string>>
        &public_reachable) {
  std::vector<std::string> out = manual_fqns;
  out.append_range(macro_reachable);
  out.append_range(alias_reachable);
  for (auto &[producer, fqns] : public_reachable)
    out.append_range(fqns);
  return out;
}

export struct MacroModuleResult {
  std::vector<std::string> macro_reachable;
  std::set<std::string> macro_modules;
  // Modules whose path says internal but whose contents say otherwise: a
  // header under `detail/` that declares names in the library's PUBLIC
  // namespace is public API filed in a subdirectory, not a hidden
  // implementation. A layout that uses `detail/` to organise files rather than
  // to hide names puts real API there — `detail/mp_list.hpp` declaring
  // `lib::mp_list` — and treating it as internal would stop the umbrella
  // re-exporting it, leaving consumers unable to name what they import.
  //
  // The namespace is what decides. A header under `detail/` declaring only
  // `lib::detail::` names stays internal, which is the case the directory rule
  // was written for.
  std::set<std::string> public_api_modules;
};

// Given a set of reachable entity FQNs, return the modules that contain them.
// A module (even an internal/impl one) whose entities are reachable must be
// treated as normal: importing modules `export import` it so consumers can use
// those entities.
export std::set<std::string> modules_with_reachable_entities(
    const std::vector<std::string> &header_paths,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args,
    const std::vector<std::string> &reachable_fqns) {
  std::set<std::string> out;
  for (auto &hp : header_paths) {
    auto m = analyze_files_with_flags({hp}, extra_args);
    bool has = false;
    for (auto &item : m.items) {
      auto fqn = fqn_of(item.ns_path, item.name);
      for (auto &rf : reachable_fqns) {
        if (matches_reachable(fqn, rf)) { has = true; break; }
      }
      if (has) break;
    }
    if (has) out.insert(derive_module_name(hp, library_name));
  }
  return out;
}

// Compute which modules have internal entities referenced by public
// macros. Such modules must be treated as normal (re-exported), even if
// they live in an internal/impl directory, because external consumers
// reach those entities via the macros. `manual_fqns` (from
// `--reachable-fqn`) are merged on top: macro analysis always runs so
// macro-reachable internals are never lost just because the user also
// listed some FQNs by hand.
// `precomputed`, when given, is the per-file entity model set for
// `header_paths` (see analyze_files_per_file), so a caller that already has
// them does not make this function extract them again.
export MacroModuleResult compute_macro_modules(
    const std::vector<std::string> &header_paths,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args,
    const std::vector<std::string> &manual_fqns = {},
    const std::vector<EntityModel> *precomputed = nullptr) {
  MacroModuleResult out;
  std::vector<EntityModel> owned;
  if (!precomputed) owned = analyze_files_per_file(header_paths, extra_args);
  const std::vector<EntityModel> &per_header =
      precomputed ? *precomputed : owned;
  EntityModel all_macros;
  for (auto &m : per_header) {
    for (auto &item : m.items) all_macros.items.push_back(item);
    for (auto &mac : m.macros) all_macros.macros.push_back(mac);
  }
  auto mr = analyze_macro_reachability(all_macros.macros, all_macros,
                                        kDefaultInternalFilter);
  expand_overloads(mr, all_macros);
  for (auto &rf : manual_fqns) mr.reachable_fqns.push_back(rf);
  expand_transitive_types(mr, all_macros);
  out.macro_reachable = std::move(mr.reachable_fqns);
  std::regex internal_ns(kDefaultInternalFilter.str());
  for (std::size_t i = 0; i < header_paths.size(); ++i) {
    auto &m = per_header[i];
    auto &hp = header_paths[i];
    bool has = false;
    bool declares_public = false;
    for (auto &item : m.items) {
      auto fqn = fqn_of(item.ns_path, item.name);
      for (auto &mrf : out.macro_reachable) {
        if (matches_reachable(fqn, mrf)) { has = true; break; }
      }
      if (!declares_public &&
          std::ranges::none_of(item.ns_path, [&](const std::string &seg) {
            return std::regex_match(seg, internal_ns);
          }))
        declares_public = true;
      if (has && declares_public) break;
    }
    auto mod = derive_module_name(hp, library_name);
    if (has) out.macro_modules.insert(mod);
    if (declares_public) out.public_api_modules.insert(std::move(mod));
  }
  return out;
}

// Second pass for cross-file macro-body export markers. A header may invoke a
// macro that is DEFINED in another library header (e.g. a declaration macro
// defined in matchers.h but invoked in more-matchers.h); the
// export markers for the entities it declares are routed to the DEFINING
// header (HeaderRewriteResult::external_macro_mods) during the first pass.
// This re-rewrites each defining header with those markers merged in
// (RewriteOptions::extra_macro_mods) so its macros file bakes the export into
// the macro body wherever it expands. `rewrite` builds the rewrite_header call
// for one header given the extra markers to merge. Returns a map from the
// defining header path to its updated macros content; callers must substitute
// these into the first-pass outcomes before writing so the marked macros files
// are not overwritten.
template <typename F>
std::map<std::string, HeaderRewriteResult>
apply_routed_macro_markers(
    const std::vector<std::string> &header_paths,
    const std::vector<HeaderRewriteResult> &results,
    const std::set<std::string> &library_headers, F &&rewrite) {
  std::map<std::string, std::vector<ModPoint>> routed;
  for (auto &r : results)
    for (auto &[target, mods] : r.external_macro_mods)
      for (auto &m : mods) routed[target].push_back(m);

  std::map<std::string, HeaderRewriteResult> updated;
  if (routed.empty()) return updated;

  std::map<std::string, std::string> canon_to_src;
  for (auto &h : header_paths) canon_to_src[canonical_path(h)] = h;

  // A single fixed-point pass suffices: routing is decided purely by spelling
  // locations, which do not change when extra_macro_mods are merged, so a
  // re-rewritten header produces the same external_macro_mods as pass one.
  std::set<std::string> processed;
  bool changed = true;
  while (changed) {
    changed = false;
    std::vector<std::pair<std::string, std::vector<ModPoint>>> pending;
    for (auto &[target, mods] : routed) {
      auto it = canon_to_src.find(target);
      if (it == canon_to_src.end() || processed.count(target)) continue;
      processed.insert(target);
      changed = true;
      auto r2 = rewrite(it->second, mods);
      if (!r2.macros_content.empty()) updated[target] = std::move(r2);
      for (auto &[t2, m2] : r2.external_macro_mods)
        pending.emplace_back(t2, m2);
    }
    for (auto &[t, ms] : pending)
      for (auto &m : ms) routed[t].push_back(m);
  }
  return updated;
}

// Everything the header-batch pipeline needs to know about the rewrite run,
// independent of any particular CLI option. Populated by the entry points;
// `rewrite_header_batch` derives per-header options from it.
export struct RewriteBatchConfig {
  std::string module_name;
  std::string library_name;
  std::set<std::string> public_modules;
  std::map<std::string, std::set<std::string>> module_replaces;
  std::vector<std::string> defined_fqns;
  std::vector<std::string> fwd_declared_fqns;
  std::set<std::string> same_module_free_fqns;
  bool combined_macros = false;
  bool extern_cxx = false;
  // Name of the macro the extern "C++" wrapping is written behind, or empty
  // for an unconditional block. See RewriteOptions::extern_cxx_macro.
  std::string extern_cxx_macro;
  bool import_std = false;
  bool cc_only = false;
  bool wrapper_module = false;
  std::string macro_prefix_override;
  bool hyphen_macros = false;
  InternalMode internal_mode = InternalMode::kBoth;
};

// Per-header outcome of the batch rewrite: the derived module name, the output
// paths (rel = subdirectory-preserving header path, rel_cc = matching .cc),
// the flat basename (`fs_stem`), and the rewrite result itself.
export struct HeaderBatchOutcome {
  std::string src;
  std::string fs_stem;
  std::string per_file_module;
  bool is_main_umbrella = false;
  std::string rel;
  std::string rel_cc;
  HeaderRewriteResult r;
  bool ok = false;
};

namespace {

RewriteOptions make_rewrite_options(const RewriteBatchConfig &cfg,
                                    llvm::StringRef src,
                                    const std::vector<std::string> &extra_args,
                                    const std::vector<std::string> &reachable_fqns,
                                    const std::set<std::string> &library_headers,
                                    const std::vector<ModPoint> &extra_macro_mods) {
  RewriteOptions o;
  o.combined_macros = cfg.combined_macros;
  o.reachable_fqns = reachable_fqns;
  o.extern_cxx = cfg.extern_cxx;
  o.extern_cxx_macro = cfg.extern_cxx_macro;
  o.extra_args = extra_args;
  o.import_std = cfg.import_std;
  o.public_modules = cfg.public_modules;
  o.internal_mode = cfg.internal_mode;
  o.module_replaces = cfg.module_replaces;
  o.defined_fqns = cfg.defined_fqns;
  o.fwd_declared_fqns = cfg.fwd_declared_fqns;
  o.cc_only = cfg.cc_only;
  o.same_module_free_fqns = cfg.same_module_free_fqns;
  o.macro_prefix_override = cfg.macro_prefix_override;
  o.hyphen_macros = cfg.hyphen_macros;
  o.umbrella_module = cfg.wrapper_module
      ? std::format("{}.umbrella", cfg.library_name)
      : std::string{};
  // The set of headers being rewritten in this run, so the export visitor
  // routes macro-body markers into the DEFINING header's macros file instead
  // of exporting at the invocation (cross-file declaration-macro handling).
  o.library_headers = library_headers;
  o.extra_macro_mods = extra_macro_mods;
  return o;
}

}  // namespace

// Rewrite a batch of headers into module interface units: per-header rewrite
// in parallel, then the cross-file macro-body export-marker routing second
// pass (updated macros content substituted back into the outcomes). `reachable_fqns`
// is the fully merged reachable set shared by every header.
export std::vector<HeaderBatchOutcome> rewrite_header_batch(
    const std::vector<std::string> &header_paths,
    const std::vector<std::string> &reachable_fqns,
    const RewriteBatchConfig &cfg,
    const clang::tooling::CompilationDatabase &cdb) {
  // Library headers being rewritten in this run, for cross-file macro-body
  // export marker routing (the export visitor resolves spelling locations
  // canonically, so canonicalize here too).
  std::set<std::string> library_headers;
  for (auto &h : header_paths) library_headers.insert(canonical_path(h));

  std::vector<HeaderBatchOutcome> outcomes(header_paths.size());
  parallel_for(header_paths.size(), [&](std::size_t idx) {
    auto &src = header_paths[idx];
    auto &o = outcomes[idx];
    o.src = src;
    o.fs_stem = std::filesystem::path(src).stem().string();
    auto extra_args = compile_extra_args(cdb, src);
    auto per_file_module = derive_module_name(src, cfg.library_name);
    // In wrapper-module mode the main umbrella header (the one named just like
    // the library) becomes `<lib>.umbrella`; a separate facade wrapper imports
    // it. Everything else keeps its derived module name.
    bool is_main_umbrella =
        cfg.wrapper_module && per_file_module == cfg.library_name;
    if (is_main_umbrella) per_file_module += ".umbrella";
    o.per_file_module = per_file_module;
    o.is_main_umbrella = is_main_umbrella;

    auto rel = header_output_relpath(src, cfg.library_name);
    // The interface unit takes the header's name with a `.cc` extension. Match
    // whatever extension the header actually has rather than `.h` alone: a
    // library spelling its headers `.hpp` would otherwise get the same path
    // back for both, and the unit written second would overwrite the first —
    // leaving an interface unit that textually includes itself.
    auto rel_cc = rel;
    auto slash = rel_cc.rfind('/');
    auto dot = rel_cc.rfind('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
      rel_cc = rel_cc.substr(0, dot);
    rel_cc += ".cc";
    // The umbrella's interface unit is written under its renamed module name so
    // it doesn't collide with the wrapper module that takes `<lib>.cc`. The file
    // uses `_umbrella.cc` / `-umbrella.cc` (matching the macro-header naming);
    // the module name itself keeps the dot (`<lib>.umbrella`).
    if (is_main_umbrella) {
      auto sep = cfg.hyphen_macros ? "-" : "_";
      rel_cc = std::format("{}{}umbrella.cc", cfg.library_name, sep);
    }
    o.rel = rel;
    o.rel_cc = rel_cc;

    auto r = rewrite_header(
        src, per_file_module,
        make_rewrite_options(cfg, src, extra_args, reachable_fqns,
                             library_headers, {}));
    o.ok = !r.h_content.empty();
    o.r = std::move(r);
  });

  // Second pass: merge export markers routed from headers that INVOKE a macro
  // into the macros file of the header that DEFINES it (e.g. a declaration
  // macro). Substitute the updated macros content into the outcomes before
  // writing so the marked macros files are not overwritten by the first-pass
  // ones.
  {
    std::vector<HeaderRewriteResult> results;
    for (auto &o : outcomes)
      if (o.ok) results.push_back(o.r);
    auto rewrite_with_markers = [&](const std::string &src,
                                    const std::vector<ModPoint> &extra) {
      auto extra_args = compile_extra_args(cdb, src);
      auto per_file_module = derive_module_name(src, cfg.library_name);
      bool is_main_umbrella =
          cfg.wrapper_module && per_file_module == cfg.library_name;
      if (is_main_umbrella) per_file_module += ".umbrella";
      return rewrite_header(
          src, per_file_module,
          make_rewrite_options(cfg, src, extra_args, reachable_fqns,
                               library_headers, extra));
    };
    auto updated = apply_routed_macro_markers(header_paths, results,
                                              library_headers,
                                              rewrite_with_markers);
    for (auto &o : outcomes) {
      auto it = updated.find(canonical_path(o.src));
      if (it != updated.end()) o.r.macros_content = it->second.macros_content;
    }
  }
  return outcomes;
}

}  // namespace modulizer
