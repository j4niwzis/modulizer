module;

#include <pthread.h>
#include <unistd.h>

export module modulizer.util;
import libtooling;
import std;

export std::string fqn_of(const std::vector<std::string> &ns_path,
                          llvm::StringRef name) {
  std::string s;
  for (auto &seg : ns_path) { s += seg; s += "::"; }
  s += name.str();
  return s;
}

export std::vector<std::string> split_on(llvm::StringRef s, char sep) {
  std::vector<std::string> out;
  for (auto &&part : s | std::views::split(sep)) {
    std::string_view sv(part);
    if (!sv.empty()) out.emplace_back(sv);
  }
  return out;
}

export std::vector<std::string> split_path(llvm::StringRef s) {
  auto split_both = s | std::views::split('/');
  std::vector<std::string> out;
  for (auto &&part : split_both) {
    std::string_view sv(part);
    if (sv.empty()) continue;
    // split_path treats both '/' and '\\' as separators.
    if (sv.find('\\') == std::string_view::npos) {
      out.emplace_back(sv);
      continue;
    }
    for (auto &&sub : sv | std::views::split('\\')) {
      std::string_view subv(sub);
      if (!subv.empty()) out.emplace_back(subv);
    }
  }
  return out;
}

// Scan source text for `ns::Name` qualified-name sequences and return every
// maximal `A::B::C` chain found (the tool's FQN convention). Shared by the
// analyzer's and header rewriter's cross-module definition scanners, which
// both fall back to scanning declaration source text when the AST cannot
// resolve dependent decltype expressions.
export std::vector<std::string> scan_fqn_text(llvm::StringRef text) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < text.size()) {
    if (std::isalnum((unsigned char)text[i]) || text[i] == '_') {
      auto j = i;
      while (j < text.size() &&
             (std::isalnum((unsigned char)text[j]) || text[j] == '_'))
        ++j;
      auto k = j;
      std::string fqn;
      while (k + 2 <= text.size() && text[k] == ':' && text[k + 1] == ':') {
        auto n = k + 2;
        auto m = n;
        while (m < text.size() &&
               (std::isalnum((unsigned char)text[m]) || text[m] == '_'))
          ++m;
        if (m == n) break;
        fqn = text.substr(i, m - i);
        k = m;
      }
      if (!fqn.empty()) out.push_back(fqn);
      i = k > j ? k : j;
    } else {
      ++i;
    }
  }
  return out;
}

// A path/module segment is "internal" if it contains an internal/impl marker.
// Note: this substring check is intentionally distinct from the exact-match
// detectors used for entities (InternalCollector, kDefaultInternalFilter) —
// unifying them would change reachability semantics.
export bool is_internal_segment(llvm::StringRef seg) {
  // A segment is internal when it is a whole `detail`/`internal`/`impl`
  // directory/name or starts with `_` (matching the kDefaultInternalFilter
  // regex). Substring matching would misclassify e.g. `mylib_pred_impl.h`
  // (a PUBLIC header whose name merely contains "impl") as internal.
  return seg == "detail" || seg == "internal" || seg == "impl" ||
         (seg.size() > 1 && seg.starts_with("_"));
}

// True when the entity FQN `fqn` matches the reference FQN `ref`: either they
// are equal, `fqn` names a member of the namespace/entity `ref` (a bare
// `Foo` reachable-fqn matching `lib::internal::Foo`), or — with `symmetric` —
// `ref` names a member of `fqn` (used when checking both a definition and its
// forward declaration against the same reachable list).
export bool matches_reachable(llvm::StringRef fqn, llvm::StringRef ref,
                              bool symmetric = false) {
  if (fqn == ref || fqn.ends_with("::" + ref.str())) return true;
  return symmetric && ref.ends_with("::" + fqn.str());
}

// Include-directory prefix of a header within its library's include tree,
// e.g. `mylib/` for `mylib/mylib-printers.h` and `mylib/internal/` for
// `mylib/internal/mylib-port.h`. The generated macro/export headers live next
// to the headers that use them, so the deployed layout mirrors the original
// headers. Empty when the library segment is not found in the path.
export std::string include_prefix(llvm::StringRef header_path,
                                  llvm::StringRef library_name) {
  auto segs = split_path(header_path);
  // The library segment must not be the last segment (there must be a header
  // basename after it).
  for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
    if (segs[i] != library_name.str()) continue;
    std::string prefix;
    for (std::size_t j = i; j + 1 < segs.size(); ++j) prefix += segs[j] + "/";
    return prefix;
  }
  return "";
}


export std::string macro_base_name(llvm::StringRef stem, bool hyphen) {
  if (!hyphen) return stem.str();
  std::string s(stem);
  std::ranges::replace(s, '_', '-');
  return s;
}

// A module that provides headers, so an include of one of them becomes an
// import of it. `std` providing <vector> and a converted library providing its
// own headers are the same arrangement described two ways, and this is the one
// description: what the module is called, which headers it stands for, and
// whether the build can turn the replacement off.
export struct ModuleReplacement {
  // The module the headers come from.
  std::string module;
  // The headers this module provides. A converted library contributes one
  // entry per module it generated — `boost.mp11.list=boost/mp11/list.hpp` —
  // rather than one entry for the whole library, so that the narrowest module
  // providing a header is the one an include of it becomes.
  std::set<std::string> headers;
  // The macro that switches the replacement on. Empty means it is on whenever
  // the module is imported at all.
  //
  // A guard changes what can be emitted. An include belongs in the global
  // module fragment and an import in the purview, so a replacement the build
  // can turn off cannot swap one for the other: the import goes under the
  // guard and the include stays under its negation. Each provider brings its
  // own, so a library is switched to modules on its own rather than every
  // library at once.
  std::string guard;
  // A module carries declarations but not macros, so a consumer that took a
  // macro from the header it no longer includes is left without it. Where the
  // provider wrote a macros file, it travels with the import.
  bool carries_macros_file = false;
};

export using ModuleReplacements = std::vector<ModuleReplacement>;

// The module replacement covering this include, if any — the most specific one
// when several do. `std` and `std.variant` both provide <variant>; the answer
// wanted is the narrower.
export const ModuleReplacement *find_replacement(
    const std::string &include_path, const ModuleReplacements &replacements) {
  const ModuleReplacement *best = nullptr;
  for (const auto &r : replacements) {
    if (!r.headers.count(include_path)) continue;
    if (!best || r.headers.size() < best->headers.size()) best = &r;
  }
  return best;
}

// Parse one --module-replaces spec:
//
//   <module>=<header>[,<header>...][:<option>...]
//
// Options are `guard=<MACRO>` and `macros`; see ModuleReplacement.
//
//   std=vector,string
//   boost.mp11.list=boost/mp11/list.hpp:guard=BOOST_MP11X_IMPORT_MODULES:macros
//
// Returns nothing and sets `error` when the spec cannot mean anything.
export std::optional<ModuleReplacement> parse_module_replacement(
    std::string_view spec, std::string &error) {
  auto eq = spec.find('=');
  if (eq == std::string_view::npos) {
    error = std::format("wants <module>=<headers>: {}", spec);
    return std::nullopt;
  }
  ModuleReplacement r;
  r.module = std::string(spec.substr(0, eq));
  if (r.module.empty()) {
    error = std::format("names no module: {}", spec);
    return std::nullopt;
  }
  auto rest = spec.substr(eq + 1);

  // Options are appended after the header list, each introduced by a colon.
  // An include path never contains one, so the split is unambiguous.
  auto opts_at = rest.find(':');
  auto header_list = rest.substr(0, opts_at);
  for (auto part : std::views::split(header_list, ',')) {
    std::string h(part.begin(), part.end());
    if (!h.empty()) r.headers.insert(std::move(h));
  }
  if (r.headers.empty()) {
    error = std::format("replaces no headers, so it replaces nothing: {}", spec);
    return std::nullopt;
  }

  while (opts_at != std::string_view::npos) {
    auto next = rest.find(':', opts_at + 1);
    auto opt = rest.substr(opts_at + 1, next == std::string_view::npos
                                            ? std::string_view::npos
                                            : next - opts_at - 1);
    opts_at = next;
    if (opt == "macros") {
      r.carries_macros_file = true;
    } else if (opt.starts_with("guard=")) {
      r.guard = std::string(opt.substr(6));
      if (r.guard.empty()) {
        // An empty guard would read as unconditional, which is the opposite of
        // what was asked for.
        error = std::format("guard= wants a macro name: {}", spec);
        return std::nullopt;
      }
    } else {
      error = std::format("unknown option '{}' in: {}", opt, spec);
      return std::nullopt;
    }
  }
  return r;
}

// Where the provider's macros file sits, given the header being replaced:
// beside it, named after its stem.
export std::string replacement_macros_header(const std::string &include_path,
                                             bool hyphen_macros) {
  auto dir = std::filesystem::path(include_path).parent_path().string();
  return (dir.empty() ? "" : dir + "/") +
         macro_base_name(std::filesystem::path(include_path).stem().string(),
                         hyphen_macros) +
         (hyphen_macros ? "-macros.h" : "_macros.h");
}


// Clang's builtin-header directory (`<clang>/lib/clang/<ver>/include`, holding
// stddef.h and friends). A ClangTool derives it from the running executable's
// path, so every parse fails with `'stddef.h' file not found` whenever the tool
// does not sit next to a clang installation — and a failed parse is silent
// here: it just yields no AST, so nothing is found to be used and every
// transitively used header is dropped from the output. Ask the clang++ on PATH
// instead, so the answer does not depend on where the binary lives.
// MODULIZER_RESOURCE_DIR overrides it.
export const std::string &clang_resource_dir() {
  static const std::string dir = [] {
    if (auto *env = std::getenv("MODULIZER_RESOURCE_DIR")) return std::string(env);
    auto tmp = std::filesystem::temp_directory_path() /
               std::format("modulizer_resdir_{}.txt", getpid());
    std::system(std::format("clang++ -print-resource-dir >{} 2>/dev/null",
                            tmp.string()).c_str());
    std::string out;
    {
      std::ifstream f(tmp);
      std::getline(f, out);
    }
    std::filesystem::remove(tmp);
    if (!out.empty() && !std::filesystem::exists(out)) out.clear();
    return out;
  }();
  return dir;
}

export std::vector<std::string> base_compile_flags(
    bool delayed_template_parsing = false) {
  std::vector<std::string> flags = {"-x", "c++", "-std=c++20",
      "-Wno-pragma-once-outside-header"};
  if (auto &rd = clang_resource_dir(); !rd.empty())
    flags.push_back(std::format("-resource-dir={}", rd));
  if (delayed_template_parsing)
    flags.push_back("-fno-delayed-template-parsing");
  return flags;
}

// Merge per-path partial FQN sets (collected by a parallel_parse pass) into a
// single sorted vector.
export std::vector<std::string> merge_sets(
    const std::vector<std::set<std::string>> &partials) {
  std::set<std::string> out;
  for (auto &p : partials) out.insert_range(p);
  return std::ranges::to<std::vector>(out);
}

// How a `parallel_parse` pass reads its inputs. Constructible from a plain
// `bool` so existing callers keep passing `/*delayed_template_parsing=*/…`.
//
// `virtual_sources` maps a path to the content to parse *instead of* the file
// on disk, keeping the path itself intact so `#include "sibling.h"` and any
// other path-relative resolution behaves exactly as for the real file. It is
// what lets a pass parse a reconstruction of a file whose on-disk form is not
// parseable on its own (see demodularize_consumer_source).
export struct ParseOptions {
  ParseOptions(bool dtp = false) : delayed_template_parsing(dtp) {}
  ParseOptions(bool dtp, const std::map<std::string, std::string> *vs)
      : delayed_template_parsing(dtp), virtual_sources(vs) {}
  bool delayed_template_parsing = false;
  const std::map<std::string, std::string> *virtual_sources = nullptr;
  // Flags for one path only, on top of the shared ones. A header written to be
  // included from one particular point does not parse anywhere else, and what
  // it is missing differs per header, so the context it needs (`-include` of
  // what its usage site had already included) cannot be shared.
  const std::map<std::string, std::vector<std::string>> *per_path_args = nullptr;
};

// Append the flags registered for this path, if any.
void add_per_path_args(std::vector<std::string> &flags, const ParseOptions &opts,
                       const std::string &path) {
  if (!opts.per_path_args) return;
  auto it = opts.per_path_args->find(path);
  if (it == opts.per_path_args->end()) return;
  flags.insert(flags.end(), it->second.begin(), it->second.end());
}

// Register every virtual source in `opts` with a tool that is about to parse.
//
// All of them, not just the file being parsed: sources include each other (a
// shared test helper header, say), so a file whose on-disk form does not parse
// breaks every parse that pulls it in, not only its own.
//
// Each is registered under the path as the caller wrote it — typically
// relative to the tree being converted — and under its absolute form, because
// ClangTool resolves its inputs to absolute paths before opening them and an
// include resolves to an absolute path too. Without both spellings the lookup
// misses and the parse quietly falls back to the file on disk.
export void apply_virtual_sources(clang::tooling::ClangTool &tool,
                                  const ParseOptions &opts) {
  if (!opts.virtual_sources) return;
  for (const auto &[path, content] : *opts.virtual_sources) {
    tool.mapVirtualFile(path, content);
    std::error_code ec;
    auto abs =
        std::filesystem::absolute(std::filesystem::path(path), ec).string();
    if (!ec && abs != path) tool.mapVirtualFile(abs, content);
  }
}

// Parse each path in `paths` in parallel with a FixedCompilationDatabase built
// from the shared extra args (plus base_compile_flags). `make_factory(i, path)`
// returns a `std::unique_ptr<clang::tooling::FrontendActionFactory>` for the
// i-th path, typically constructed over per-index partial state captured by
// reference (which must outlive the tool.run call — it does, since the factory
// is used within the same worker iteration). The optional `finalize(i, path)`
// runs after tool.run inside the same worker, for per-path post-processing
// (e.g. classifying parsed entities into partial maps).
export template <typename MakeFactory>
void parallel_parse(const std::vector<std::string> &paths,
                    const std::vector<std::string> &extra_args,
                    ParseOptions opts,
                    MakeFactory make_factory) {
  parallel_for(paths.size(), [&](std::size_t i) {
    auto &path = paths[i];
    std::vector<std::string> sources = {path};
    auto flags = base_compile_flags(opts.delayed_template_parsing);
    flags.insert(flags.end(), extra_args.begin(), extra_args.end());
    add_per_path_args(flags, opts, path);
    clang::tooling::FixedCompilationDatabase db(".", flags);
    clang::tooling::ClangTool tool(db, sources);
    apply_virtual_sources(tool, opts);
    auto factory = make_factory(i, path);
    tool.run(factory.get());
  });
}

export template <typename MakeFactory, typename Finalize>
void parallel_parse(const std::vector<std::string> &paths,
                    const std::vector<std::string> &extra_args,
                    ParseOptions opts, MakeFactory make_factory,
                    Finalize finalize) {
  parallel_for(paths.size(), [&](std::size_t i) {
    auto &path = paths[i];
    std::vector<std::string> sources = {path};
    auto flags = base_compile_flags(opts.delayed_template_parsing);
    flags.insert(flags.end(), extra_args.begin(), extra_args.end());
    add_per_path_args(flags, opts, path);
    clang::tooling::FixedCompilationDatabase db(".", flags);
    clang::tooling::ClangTool tool(db, sources);
    apply_virtual_sources(tool, opts);
    auto factory = make_factory(i, path);
    tool.run(factory.get());
    finalize(i, path);
  });
}

// Run `fn(i)` for i in [0, n) across a bounded number of worker threads. The
// analysis/rewrite passes are clang-parse bound; parallelism keeps them
// tractable for large header/source sets (e.g. a library's own test suite).
//
// Each worker is created with a large stack: clang's Sema recurses deeply when
// instantiating templates, and the default pthread stack (8 MiB) overflows on
// the worker threads. pthread_create is used (not std::thread) because the
// standard library offers no way to size a thread's stack.
export void parallel_for(std::size_t n,
                         const std::function<void(std::size_t)> &fn) {
  unsigned workers = std::max(1u, std::thread::hardware_concurrency());
  workers = std::min<unsigned>(workers, static_cast<unsigned>(n));
  if (workers <= 1) {
    for (std::size_t i = 0; i < n; ++i) fn(i);
    return;
  }
  struct Shared {
    std::atomic<std::size_t> next{0};
    std::size_t n = 0;
    const std::function<void(std::size_t)> *fn = nullptr;
  } shared;
  shared.n = n;
  shared.fn = &fn;
  std::vector<pthread_t> tids(workers);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512ull * 1024 * 1024);
  for (unsigned w = 0; w < workers; ++w) {
    pthread_create(&tids[w], &attr, [](void *arg) -> void * {
      auto *s = static_cast<Shared *>(arg);
      for (;;) {
        std::size_t i = s->next.fetch_add(1);
        if (i >= s->n) break;
        (*s->fn)(i);
      }
      return nullptr;
    }, &shared);
  }
  pthread_attr_destroy(&attr);
  for (auto &t : tids) pthread_join(t, nullptr);
}
