module;

export module modulizer.rewrite_includes;
import modulizer.consumer_rewriter;
import modulizer.include_analysis;
import modulizer.naming;
import modulizer.util;
import libtooling;
import std;

// Include-directive classification and GMF/import emission for the header
// rewriter: which quoted includes become module imports, which system includes
// are replaced/kept, and how the global module fragment blocks are built.

// True when `resolved` (an absolute path) lives under one of the compiler's
// system include directories — i.e. it is a third-party/system header, not a
// project-local sibling library header.

// Canonicalized path, matching how the set of headers being rewritten is keyed
// (both sides must agree or a resolved include never matches one of ours).
std::string canonical_include_path(llvm::StringRef p) {
  std::error_code ec;
  auto c = std::filesystem::weakly_canonical(std::filesystem::path(p.str()), ec);
  return ec ? p.str() : c.string();
}

// Result of classifying a quoted (non-`custom/`) library include: whether it is
// a library header that must become an import, and the module it maps to.
export struct LibIncludeClass {
  bool is_lib_include = false;
  std::string derived;
};

// Classify a quoted library include (direct or transitive): is it a header of
// this library or of a project-local sibling library (auto-derived from the
// first path segment as the library root), and which module does it become?
// `custom/` headers (e.g. mylib/internal/custom/mylib-port.h) are
// user-provided and are never classified as library includes. A cross-library
// include must resolve to a project-local file, not a third-party/system
// header (e.g. a third_party dep pulled in by a library). Callers are responsible for the umbrella-module
// rename.
export LibIncludeClass classify_library_include(
    const IncludeDirective &inc, llvm::StringRef library_name,
    llvm::StringRef header_path, const std::vector<std::string> &extra_args,
    // The headers this run is rewriting, canonicalized; see compute_auto_imports.
    const std::set<std::string> *converted = nullptr) {
  auto segs = split_path(inc.path);
  auto root = segs.empty() ? "" : segs.front();
  // Under our own include prefix the converted set is the whole truth: a path
  // that does not resolve into it is not a module of ours, however much it
  // looks like one. Without this a library name that is a VENDOR prefix claims
  // every library sharing it — `boost.system` makes the library `boost`, and
  // Boost.Config's headers then resolve to modules nobody generated.
  if (converted && root == library_name.str()) {
    auto resolved = resolve_include(inc.path, header_path.str(), extra_args);
    if (resolved.empty() || !converted->count(canonical_include_path(resolved)))
      return {};
  }
  auto derived = derive_module_name(inc.path, library_name);
  auto lib_prefix = std::format("{}/", library_name.str());
  bool is_lib_include =
      !inc.path.contains("/custom/") &&
      (inc.path.starts_with(lib_prefix) || derived == library_name.str() ||
       (root != library_name.str() && !is_internal_segment(root) &&
        inc.guard_stack.empty()));
  if (is_lib_include && root != library_name.str() &&
      !is_internal_segment(root)) {
    // A cross-library include must resolve to a project-local file, not a
    // third-party/system header (e.g. a third_party dep pulled in by a library).
    auto resolved = resolve_include(inc.path, header_path.str(), extra_args);
    if (resolved.empty() || resolved_in_system_dir(resolved))
      is_lib_include = false;
    else
      derived = derive_module_name(inc.path, root);
  }
  return {is_lib_include, std::move(derived)};
}

// Compute auto-imports: library-internal includes become module imports
// (unless they are the umbrella module or the module itself).
//
// `converted` is the set of headers this run is rewriting, canonicalized. An
// include that resolves into it is one of ours whatever it looks like, and
// that is the answer we trust: quoting style and path shape are conventions,
// not facts. Google style writes `#include "lib/thing.h"` and puts the library
// first; a great many libraries write `#include <vendor/lib/thing.hpp>` and put
// it second. Deciding from the spelling alone left the second kind with no
// imports at all — every sibling stayed a textual include in the global module
// fragment, which is not a conversion.
//
// The spelling-based rules below still stand for what `converted` cannot
// answer: a header of a SIBLING library, which this run is not rewriting and so
// never appears in that set.
export std::map<std::string, std::string> compute_auto_imports(
    const std::vector<IncludeDirective> &includes,
    llvm::StringRef library_name, llvm::StringRef module_name,
    llvm::StringRef header_path, const std::vector<std::string> &extra_args,
    std::vector<std::pair<std::string, std::string>> &imported_modules,
    std::set<std::string> &extra_macro_includes, bool hyphen_macros,
    const std::set<std::string> *converted = nullptr) {
  std::map<std::string, std::string> auto_imports;
  // Records the import and the macro header that must come with it: macros do
  // not cross a module boundary, so an imported module's macros have to arrive
  // as an include or every use of one is an undefined-macro error.
  auto take = [&](const IncludeDirective &inc, const std::string &mod) {
    auto_imports[inc.path] = mod;
    imported_modules.push_back({inc.path, mod});
    auto sep = hyphen_macros ? "-" : "_";
    auto dir = std::filesystem::path(inc.path).parent_path().string();
    extra_macro_includes.insert(
        (dir.empty() ? "" : dir + "/") +
        macro_base_name(std::filesystem::path(inc.path).stem().string(),
                        hyphen_macros) +
        sep + "macros.h");
  };

  for (const auto &inc : includes) {
    if (is_xmacro_include(inc.path)) continue;
    // A guard does not disqualify one of ours. The rule below — that a guarded
    // include cannot become an unconditional import — is about headers that may
    // not exist unless the condition holds (`#if LIB_HAS_ABSL` around an absl
    // header). A header this run is converting always has a module, whatever
    // the condition says, and leaving it as an include instead means the
    // generated tree cannot be built without the ORIGINAL headers on the
    // include path:
    //
    //   fatal error: 'lib/thing.hpp' file not found
    if (converted && !inc.path.contains("/custom/")) {
      auto resolved = resolve_include(inc.path, header_path.str(), extra_args);
      if (!resolved.empty() && converted->count(canonical_include_path(resolved))) {
        auto mod = derive_module_name(inc.path, library_name);
        if (mod == module_name.str()) continue;
        // The umbrella of the CURRENT library would be a cycle.
        if (mod == library_name.str() &&
            module_name.str().starts_with(library_name.str() + "."))
          continue;
        take(inc, mod);
        continue;
      }
    }
    if (!inc.is_quoted) continue;
    // A guarded include (e.g. `#if LIB_HAS_ABSL` around an absl header)
    // cannot become an unconditional import, so it is not auto-derived.
    if (!inc.guard_stack.empty()) continue;
    // Auto-generate the replace list for a quoted library include whose first
    // path segment is a library root. This covers both this library's own
    // headers (`{library}/...`) and headers of another library (e.g. a sibling
    // including `other/other.h` → module `other`): the first segment names the
    // library, and the module name is derived from the path after it. The
    // umbrella module of the *current* library is skipped to avoid circular
    // deps, but a *different* library's umbrella is imported.
    auto segs = split_path(inc.path);
    if (segs.empty()) continue;
    auto root = segs.front();
    // Under our own include prefix the set above is the whole truth: if the
    // include did not resolve into it, there is no module of ours behind this
    // path and the spelling rule must not invent one. It otherwise does, every
    // time the library name is a VENDOR prefix shared with other libraries —
    // `boost.system` makes the library `boost`, and then every `boost/...`
    // include, Boost.Config's among them, looks like one of ours:
    //
    //   fatal error: module 'boost.config.compiler.nvcc' not found
    //
    // A SIBLING library keeps its own root (`other/other.h`) and is unaffected.
    if (converted && root == library_name.str()) continue;
    if (root != library_name.str() && is_internal_segment(root)) continue;
    if (inc.path.contains("/custom/")) continue;
    // A header of a *different* library must resolve to a project-local file,
    // not a third-party/system header (e.g. a dep pulled in by a library):
    // those are not sibling libraries and must stay as ordinary includes.
    if (root != library_name.str()) {
      auto resolved = resolve_include(inc.path, header_path, extra_args);
      if (resolved.empty()) continue;
      if (resolved_in_system_dir(resolved)) continue;
    }
    auto mod_name = derive_module_name(inc.path, root);
    if (root == library_name.str() && mod_name == module_name.str()) continue;
    // Skip importing the current library's umbrella module from sub-modules to
    // avoid circular deps.
    if (root == library_name.str() && mod_name == library_name.str() &&
        module_name.str().starts_with(library_name.str() + "."))
      continue;
    // The include directive already carries the header's directory within its
    // library's include tree (`mylib/internal/mylib-port.h` → `mylib/internal/`),
    // so the macro file lives there too.
    take(inc, mod_name);
  }
  return auto_imports;
}

// Classify whether an include is provided by an imported module. When the
// module is std or std.compat, the C++ stdlib headers it replaces (plus
// libc++ internal __-prefixed headers reachable from them) are emitted
// guarded by <LIB>_IMPORT_STD instead of being dropped, so the module still
// compiles without import std. Headers replaced by any other module are
// removed outright. C headers (stdio.h, cstdio, ...) are never replaced —
// they provide POSIX names not re-exported by the std module.
export struct ReplacementClass {
  bool other_replaced = false;
  bool std_replaced = false;
};

export ReplacementClass classify_replacement(
    const IncludeDirective &inc, llvm::StringRef header_path,
    const std::vector<std::string> &extra_args,
    const std::set<std::string> &imported_modules,
    const std::map<std::string, std::set<std::string>> &module_replaces) {
  ReplacementClass rc;
  auto replaced_by = [&](const std::string &hdr) {
    // Feature-test headers like `<version>` (and `<compare>`) only define
    // preprocessor macros (`__cpp_lib_char8_t`, ...), which `import std;` can
    // never provide. They must stay in the GMF even under import std, so they
    // are never std-replaced.
    if (hdr == "version" || hdr == "compare") return;
    for (auto &[mod, set] : module_replaces) {
      if (imported_modules.count(mod) && set.count(hdr)) {
        if (mod == "std" || mod == "std.compat") rc.std_replaced = true;
        else rc.other_replaced = true;
      }
    }
  };
  if (!inc.is_quoted) {
    replaced_by(inc.path);
    if (!rc.std_replaced &&
        (imported_modules.count("std") ||
         imported_modules.count("std.compat"))) {
      // `import std.compat;` provides every C++ standard-library header, so
      // they are dropped from the GMF. The C headers they wrap (stdio.h,
      // stdlib.h, ...) are NOT replaced — they carry macros (stderr, errno,
      // PTHREAD_*) that a module cannot provide — and neither are the
      // `cstdio`/`cerrno`/`cassert` wrappers, whose macros (stderr, errno,
      // assert) the code relies on. The feature-test headers
      // `<version>`/`<compare>` only define macros and are never std-replaced
      // (see replaced_by).
      if (kStdHeaders.count(inc.path) && inc.path != "cstdio" &&
          inc.path != "cerrno" && inc.path != "cassert")
        rc.std_replaced = true;
      auto resolved = resolve_include(inc.path, header_path, extra_args);
      auto cpp_inc = std::filesystem::path(
          get_system_include_dirs().empty()
              ? "" : get_system_include_dirs()[0]).lexically_normal().string();
      if (!resolved.empty() && !cpp_inc.empty()) {
        auto rp = std::filesystem::path(resolved).lexically_normal().string();
        if (rp.starts_with(cpp_inc)) {
          auto rel = rp.substr(cpp_inc.size());
          // libc++ internals live in __-prefixed files/directories.
          auto first = rel.find_first_not_of("/\\");
          if (first != std::string::npos &&
              rel[first] == '_' && rel[first + 1] == '_')
            rc.std_replaced = true;
        }
      }
    }
  }
  return rc;
}

// The std module name to emit for the `--import-std` feature. When the
// `--module-replaces` list targets `std.compat` (which, unlike `std`, also
// provides the C library's global names — `size_t`, `printf`, ... — that the
// converted code relies on), emit that; otherwise plain `std`. C macros
// (`errno`, `assert`, ...) are never provided by either module and still come
// from the C headers kept in the global module fragment.
export std::string std_module_name(
    const std::map<std::string, std::set<std::string>> &module_replaces) {
  for (auto &[mod, set] : module_replaces)
    if (mod == "std.compat") return "std.compat";
  return "std";
}

// Guards for an include that are valid to emit in the GMF (skip \ continuations).
export std::vector<std::string> valid_guards(const std::vector<std::string> &guards) {
  std::vector<std::string> out;
  for (auto &g : guards) {
    auto trimmed = std::string_view(g);
    auto ns = trimmed.find_first_not_of(" \t");
    if (ns != std::string_view::npos) trimmed.remove_prefix(ns);
    if (!trimmed.empty() && trimmed.back() == '\n')
      trimmed.remove_suffix(1);
    if (!trimmed.empty() && trimmed.back() != '\\')
      out.push_back(g);
  }
  return out;
}

// Emit GMF includes into cc, merging adjacent identical guards.
export void emit_gmf_blocks(std::string &cc,
                            const std::vector<IncludeDirective> &gmf_incs) {
  // libc++ internal headers (`__config`, `__cxx03/version`, `__vector/...`)
  // are implementation details reached only through the public C++ headers.
  // The GMF either keeps those public headers textually (plain mode pulls the
  // internals in itself) or replaces them with `import std.compat;` (the
  // internals are not needed at all), so they are never emitted.
  auto is_libcpp_internal = [](llvm::StringRef path) {
    llvm::SmallVector<llvm::StringRef, 8> parts;
    path.split(parts, '/');
    for (auto &p : parts)
      if (p.size() >= 2 && p[0] == '_' && p[1] == '_') return true;
    return false;
  };
  // The same header is frequently reached through several parents, each with
  // its own guard context; a header also has its own include guard, so a
  // single textual include per (header, guard-context) suffices.
  auto guards_key = [](const std::vector<std::string> &gs) {
    std::string k;
    for (auto &g : gs) k += g;
    return k;
  };
  // Collapse repeated consecutive guard directives. `#ifndef X` nested inside
  // an outer `#ifndef X` is redundant (the condition already holds), so the
  // inner guard and its matching `#endif` are dropped — the transitive include
  // collection frequently records the same guard twice.
  auto collapse_guards = [](const std::vector<std::string> &gs) {
    std::vector<std::string> out;
    for (auto &g : gs) {
      auto trimmed = std::string_view(g);
      auto ns = trimmed.find_first_not_of(" \t");
      if (ns != std::string_view::npos) trimmed.remove_prefix(ns);
      if (out.empty() || out.back() != g) {
        out.push_back(g);
        continue;
      }
      if (trimmed.starts_with("#if"))
        continue;  // nested identical conditional: drop the duplicate
      out.push_back(g);
    }
    return out;
  };
  // Group ALL includes by identical guard context (not just adjacent ones),
  // preserving the order of first appearance of each context and the include
  // order within it. Guard conditions are independent preprocessor contexts
  // for system headers, so interleaved same-guard blocks can be merged.
  std::set<std::pair<std::string, std::string>> seen;
  std::vector<std::string> keys;
  std::map<std::string, std::vector<const IncludeDirective *>> groups;
  std::map<std::string, std::vector<std::string>> key_guards;
  std::set<std::string> seen_keys;
  for (auto &inc : gmf_incs) {
    if (is_libcpp_internal(inc.path)) continue;
    auto cur_guards = valid_guards(inc.guard_stack);
    auto gk = guards_key(cur_guards);
    if (!seen.insert(std::make_pair(inc.path, gk)).second) continue;
    if (seen_keys.insert(gk).second) {
      keys.push_back(gk);
      key_guards[gk] = cur_guards;
    }
    groups[gk].push_back(&inc);
  }
  for (auto &gk : keys) {
    auto cg = collapse_guards(key_guards[gk]);
    for (auto &g : cg) cc += g;
    for (auto *inc : groups[gk]) cc += inc->line;
    for (std::size_t k = 0; k < cg.size(); ++k) cc += "#endif\n";
  }
}

// Wrap include directives in hdr with #ifndef USE_MODULES.
// Skip includes that are inside #ifdef blocks (they keep original guards).
export std::string wrap_includes_with_guard(
    std::string hdr, const std::vector<IncludeDirective> &includes,
    const std::string &use_modules_macro, bool remove = false) {
  auto find_inc = [](const std::vector<IncludeDirective> &incs,
                     const std::string &path) -> const IncludeDirective * {
    for (auto &i : incs) if (i.path == path) return &i;
    return nullptr;
  };
  std::string wrapped_hdr;
  std::size_t pos = 0;
  while (pos < hdr.size()) {
    auto nl = hdr.find('\n', pos);
    if (nl == std::string::npos) nl = hdr.size() - 1;
    auto line = std::string_view(hdr).substr(pos, nl - pos + 1);
    bool is_include = false;
    std::string inc_path;
    auto inc = parse_include_line(line, /*skip_hash_ws=*/false);
    if (inc && !is_xmacro_include(inc->path)) {
      inc_path = std::move(inc->path);
      is_include = true;
    }
    if (is_include) {
      auto *inc = find_inc(includes, inc_path);
      bool has_conditional_guard = inc && inc->guard_stack.size() > 1;
      if (inc && (inc->skip_gmf || has_conditional_guard)) {
        wrapped_hdr += line;  // keep original guards
      } else if (remove) {
        // Drop the include entirely (cc-only mode: USE_MODULES is always
        // defined in the interface unit, so these includes are dead — the
        // library headers are imported and system headers live in the GMF).
      } else {
        wrapped_hdr += std::format("#ifndef {}\n", use_modules_macro);
        wrapped_hdr += line;
        wrapped_hdr += "#endif\n";
      }
    } else {
      wrapped_hdr += line;
    }
    pos = nl + 1;
  }
  hdr = std::move(wrapped_hdr);

  // Merge adjacent #ifndef USE_MODULES blocks
  auto merge_from = std::format("#endif\n#ifndef {}\n", use_modules_macro);
  for (std::size_t p = 0;
       (p = hdr.find(merge_from, p)) != std::string::npos; ) {
    hdr.erase(p, merge_from.size());
  }
  return hdr;
}
