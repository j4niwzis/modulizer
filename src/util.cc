module;

#include <pthread.h>

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

// The base name of a generated macro header for a header stem, e.g. the stem
// `mylib_pred_impl` stays `mylib_pred_impl` (underscore style) or becomes
// `mylib-pred-impl` (--hyphen-macros style).
export std::string macro_base_name(llvm::StringRef stem, bool hyphen) {
  if (!hyphen) return stem.str();
  std::string s(stem);
  std::ranges::replace(s, '_', '-');
  return s;
}

export std::vector<std::string> base_compile_flags(
    bool delayed_template_parsing = false) {
  std::vector<std::string> flags = {"-x", "c++", "-std=c++20",
      "-Wno-pragma-once-outside-header"};
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

// Parse each path in `paths` in parallel with a FixedCompilationDatabase built
// from the shared extra args (plus base_compile_flags). `make_factory(i, path)`
// returns a `std::unique_ptr<clang::tooling::FrontendActionFactory>` for the
// i-th path, typically constructed over per-index partial state captured by
// reference (which must outlive the tool.run call — it does, since the factory
// is used within the same worker iteration). The optional `finalize(i, path)`
// runs after tool.run inside the same worker, for per-path post-processing
// (e.g. classifying parsed entities into partial maps).
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
};

// Apply `opts.virtual_sources` (if any) to a tool about to parse `path`.
export void apply_virtual_source(clang::tooling::ClangTool &tool,
                                 const std::string &path,
                                 const ParseOptions &opts) {
  if (!opts.virtual_sources) return;
  auto it = opts.virtual_sources->find(path);
  if (it == opts.virtual_sources->end()) return;
  tool.mapVirtualFile(path, it->second);
}

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
    clang::tooling::FixedCompilationDatabase db(".", flags);
    clang::tooling::ClangTool tool(db, sources);
    apply_virtual_source(tool, path, opts);
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
    clang::tooling::FixedCompilationDatabase db(".", flags);
    clang::tooling::ClangTool tool(db, sources);
    apply_virtual_source(tool, path, opts);
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
