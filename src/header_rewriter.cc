module;

export module modulizer.header_rewriter;
import modulizer.analyzer;
import modulizer.astutil;
import modulizer.include_analysis;
import modulizer.naming;
import modulizer.rewrite_export;
import modulizer.rewrite_impls;
import modulizer.rewrite_includes;
import modulizer.rewrite_macros;
import modulizer.rewrite_util;
import modulizer.rewrite_visitors;
import modulizer.util;
import libtooling;
import std;

// Entry points for the header/source rewrite. All the per-file machinery
// (export annotation, macro collection, include classification, forward-decl
// injection, impl-unit extern "C++" collection) lives in the modulizer.rewrite_*
// modules; this module drives it and assembles the generated .h/.cc outputs.

export struct HeaderRewriteResult {
  std::string h_content;
  std::string cc_content;
  std::string macros_content;
  std::string macros_name;
  std::string export_h_content;
  std::string export_h_name;
  std::string module_name;
  std::vector<std::string> internal_fqns;
  std::vector<std::pair<std::string, std::string>> imported_modules;
  // Export markers computed while rewriting THIS header whose spelling location
  // lands inside a macro body in ANOTHER library header being rewritten (e.g.
  // a macro defined in matchers.h but invoked in more-matchers.h). Keyed by the
  // defining header's path; the CLI
  // aggregates them and re-runs the defining header with them as
  // RewriteOptions::extra_macro_mods so its macros file bakes them in.
  std::map<std::string, std::vector<ModPoint>> external_macro_mods;
};

// Options for rewrite_header. Use designated initializers:
//   rewrite_header(path, "mylib", {.reachable_fqns = {...}, .cc_only = true});
export struct RewriteOptions {
  bool combined_macros = false;
  std::map<std::string, std::string> include_to_module;
  std::vector<std::string> reachable_fqns;
  bool extern_cxx = true;
  // When set, the `extern "C++"` block wrapping the purview is written behind
  // `#ifdef <this macro>` rather than unconditionally. One generated tree then
  // serves both compilers: left undefined the entities keep module linkage,
  // which is what a conforming implementation wants; defined, everything is
  // given C++ language linkage in the global module, which is what GCC's
  // modules need to link against implementations that are not module units.
  std::string extern_cxx_macro;
  std::vector<std::string> extra_args;
  bool no_internal_filter = false;
  bool import_std = false;
  std::set<std::string> public_modules;
  InternalMode internal_mode = InternalMode::kBoth;
  std::map<std::string, std::set<std::string>> module_replaces;
  std::vector<std::string> defined_fqns;
  std::vector<std::string> fwd_declared_fqns;
  bool cc_only = false;
  std::set<std::string> same_module_free_fqns;
  std::string macro_prefix_override;
  bool hyphen_macros = false;
  // In wrapper-module mode the umbrella module is renamed to <lib>.umbrella;
  // sub-modules that include the umbrella header must import the renamed
  // module, not the wrapper facade module that takes the bare library name.
  std::string umbrella_module;
  // Library headers being rewritten in this run (absolute paths). The export
  // visitor routes macro-body export markers whose spelling location is inside
  // one of these OTHER headers into RewriteOptions::external_macro_mods (see
  // HeaderRewriteResult), so the defining header's macros file bakes them in.
  std::set<std::string> library_headers;
  // Export markers (from HeaderRewriteResult::external_macro_mods of other
  // headers) to bake into THIS header's macros file. Merged into `mods` before
  // build_macros_file so macro bodies whose entities are invoked elsewhere
  // carry the export marker wherever they expand.
  std::vector<ModPoint> extra_macro_mods;
};

namespace {

// The GMF include list plus the purview imports derived from a header's
// include closure (see classify_includes).
struct GmfAndImports {
  std::vector<IncludeDirective> gmf_incs;
  std::vector<std::string> purview_imports;
};

// Classify every include directive into either a GMF include (system /
// third-party headers the module textually pulls in) or a purview module
// import (library headers replaced by `import`). Mutates `imported_modules`
// with every module this unit (transitively) depends on, which
// `classify_replacement` uses to drop headers provided by an imported module.
// `includes` is moved-from: entries that become GMF includes are moved into
// the result.
GmfAndImports classify_includes(
    std::vector<IncludeDirective> &includes,
    const std::map<std::string, std::string> &auto_imports,
    llvm::StringRef library_name, llvm::StringRef module_name,
    llvm::StringRef header_path, const RewriteOptions &options,
    std::set<std::string> &imported_modules,
    const std::set<std::string> &used_headers,
    const std::string &std_import_guard) {
  GmfAndImports out;
  for (auto &inc : includes) {
    if (is_xmacro_include(inc.path)) continue;
    // A header explicitly provided by a module (`--module-replaces`): import
    // that module instead of keeping the include. Non-std modules replace the
    // include outright; std/std.compat only mark it for guarded removal because
    // `import std;` is emitted separately by the `--import-std` flag. When
    // several modules replace the same header, prefer the most specific one
    // (the module replacing the fewest headers, e.g. `std.variant` over
    // `std` for `<variant>`).
    std::string replaced_mod =
        find_replacing_module(inc.path, options.module_replaces);
    if (!replaced_mod.empty() && replaced_mod != "std" &&
        replaced_mod != "std.compat") {
      out.purview_imports.push_back(format_import(
          inc.path, replaced_mod, options.public_modules, options.internal_mode));
      imported_modules.insert(replaced_mod);
      continue;
    }
    auto it = options.include_to_module.find(inc.path);
    auto ait = auto_imports.find(inc.path);
    if (it != options.include_to_module.end()) {
      auto &mname = it->second;
      out.purview_imports.push_back(format_import(
          inc.path, mname, options.public_modules, options.internal_mode));
    } else if (ait != auto_imports.end()) {
      auto &mname = ait->second;
      out.purview_imports.push_back(format_import(
          inc.path, mname, options.public_modules, options.internal_mode));
    } else if (!inc.skip_gmf) {
      // A direct include of another library header that auto-imports did not
      // cover — including the library's umbrella/root header. It must be
      // imported, not kept in the GMF: a GMF textual include would define the
      // library's entities in the global module and conflict with the imported
      // modules. (A header both included by the umbrella and including it would
      // already be a textual include cycle.)
      if (inc.is_quoted && !inc.transitive &&
          !inc.path.contains("/custom/")) {
        auto cl = classify_library_include(inc, library_name, header_path,
                                           options.extra_args);
        if (cl.is_lib_include) {
          // In wrapper-module mode the umbrella module is renamed; sub-modules
          // that include the umbrella header must import the renamed module,
          // not the wrapper facade that takes the bare library name.
          if (cl.derived == library_name.str() &&
              !options.umbrella_module.empty())
            cl.derived = options.umbrella_module;
          out.purview_imports.push_back(format_import(
              inc.path, cl.derived, options.public_modules,
              options.internal_mode));
          continue;
        }
      }
      // Transitive system includes (reached through quoted library headers
      // that become module imports) only stay in the GMF if the module's
      // own code references a declaration from them. With the compiler's
      // system include paths available, every real header resolves; anything
      // unresolvable (e.g. inactive libc++ __cxx03 internals) is dropped.
      if (inc.transitive) {
        // A standard-library private header: never emitted (its use, if any,
        // was credited to its public ancestor by used_public_ancestors).
        if (inc.system_internal) continue;
        if (!inc.is_quoted) {
          auto resolved =
              resolve_include(inc.path, header_path.str(), options.extra_args);
          if (!keep_transitive_system_include(resolved, inc.path,
                                              used_headers)) {
            // `<version>` defines only preprocessor macros the header body
            // evaluates (e.g. `__cpp_lib_char8_t`); macros are not tracked by
            // used_headers, but the header must still be present for the code
            // to compile. (Only `<version>`: `<compare>` pulls libc++'s
            // __cxx03/compare which breaks dependency scanning.)
            continue;
          }
          if (!resolved.empty()) {
            auto rc = classify_replacement(inc, header_path, options.extra_args,
                                           imported_modules,
                                           options.module_replaces);
            if (rc.other_replaced) continue;
            if (rc.std_replaced)
              inc.guard_stack.insert(inc.guard_stack.begin(),
                                     "#ifndef " + std_import_guard + "\n");
            if (inc.skip_gmf) inc.guard_stack.clear();
            out.gmf_incs.push_back(inc);
          }
        } else {
          // Transitive quoted library include: the header it comes from became
          // an import (directly or via the umbrella), so a GMF textual include
          // would define the library's entities in the global module and
          // conflict with the imported modules. This covers both this library's
          // headers and cross-library headers (auto-derived from the first path
          // segment as the library root). The consumer's own code may use the
          // transitively-reached entities directly (the umbrella does not
          // re-export every sub-module), so import the derived module too.
          auto cl = classify_library_include(inc, library_name, header_path,
                                             options.extra_args);
          if (!cl.is_lib_include) continue;
          // In wrapper-module mode the umbrella module is renamed; a sub-module
          // that transitively reaches the umbrella header imports the renamed
          // module, not the wrapper facade that takes the bare library name.
          if (cl.derived == library_name.str() &&
              !options.umbrella_module.empty())
            cl.derived = options.umbrella_module;
          if (cl.derived == module_name.str()) continue;
          out.purview_imports.push_back(format_import(
              inc.path, cl.derived, options.public_modules,
              options.internal_mode));
          imported_modules.insert(cl.derived);
          continue;
        }
      }
      // Headers provided by an imported module: see classify_replacement.
      auto rc = classify_replacement(inc, header_path, options.extra_args,
                                     imported_modules, options.module_replaces);
      if (rc.other_replaced) continue;
      if (rc.std_replaced) {
        inc.guard_stack.insert(inc.guard_stack.begin(),
                               "#ifndef " + std_import_guard + "\n");
      }
      out.gmf_incs.push_back(std::move(inc));
    }
  }
  return out;
}

// Whether a conditional block defines its macros on every path, so that what it
// defines is what every platform gets. A block with an `#else` at its own level
// does; a one-sided `#if defined(__clang__)` does not — it overrides the
// definitions preceding it and leaves every other compiler with those, which is
// exactly the `#undef X` + redefine idiom.
bool block_covers_every_platform(std::string_view body) {
  int depth = 0;
  std::size_t pos = 0;
  while (pos < body.size()) {
    auto nl = body.find('\n', pos);
    if (nl == std::string_view::npos) nl = body.size();
    auto line = body.substr(pos, nl - pos);
    auto ns = line.find_first_not_of(" \t");
    if (ns != std::string_view::npos) {
      auto d = parse_directive(line);
      auto kw = d ? std::string_view(d->keyword) : std::string_view();
      if (kw.starts_with("if")) ++depth;
      else if (kw == "endif") --depth;
      else if (depth == 1 && kw == "else") return true;
    }
    pos = nl + 1;
  }
  return false;
}

// Source ranges of the conditional blocks emitted verbatim. A `#define` inside
// one of them travels with its block and must not be emitted a second time as a
// bare definition — which would drop its condition and hand every platform the
// branch that happened to be active while converting.
std::vector<std::pair<unsigned, unsigned>> collect_block_ranges(
    const std::vector<MacroRec> &macros) {
  std::vector<std::pair<unsigned, unsigned>> ranges;
  for (auto &m : macros)
    if (m.name.empty() && m.end_off > m.start_off)
      ranges.push_back({m.start_off, m.end_off});
  return ranges;
}

bool inside_block(const std::vector<std::pair<unsigned, unsigned>> &ranges,
                  const MacroRec &m) {
  for (auto &[start, end] : ranges)
    if (m.start_off >= start && m.end_off <= end) return true;
  return false;
}

// Names of macros (re)defined inside an emitted conditional block that covers
// every platform. Such a block is authoritative and re-defining the macro as a
// bare unconditional `#define` would cause "macro redefined" diagnostics. A
// one-sided block is not: the definitions it overrides are what the other
// platforms use, so they must reach the macros file as well.
std::set<std::string> collect_block_defined_names(
    const std::vector<MacroRec> &macros) {
  std::set<std::string> names;
  for (auto &m : macros) {
    if (!m.name.empty()) continue;
    if (!block_covers_every_platform(m.body)) continue;
    std::string_view body(m.body);
    std::size_t pos = 0;
    while (pos < body.size()) {
      auto nl = body.find('\n', pos);
      if (nl == std::string_view::npos) nl = body.size();
      auto line = body.substr(pos, nl - pos);
      auto ns = line.find_first_not_of(" \t");
      auto d = parse_directive(line);
      if (ns != std::string_view::npos && d && d->keyword == "define") {
        auto rest = std::string_view(d->after);
        auto name_start = rest.find_first_not_of(" \t");
        if (name_start != std::string_view::npos) {
          auto name_end = rest.find_first_of("( \t", name_start);
          names.insert(std::string(
              rest.substr(name_start,
                          name_end == std::string_view::npos
                              ? std::string_view::npos
                              : name_end - name_start)));
        }
      }
      pos = nl + 1;
    }
  }
  return names;
}

// Push removal mods for every public macro that moves to the macros file
// (stripping its guard/`#define` from the header body) and return the source
// ranges they cover. The visitor's marker insertions landing inside those
// ranges belong to the macros file, not the header body.
std::vector<std::pair<unsigned, unsigned>> collect_stripped_ranges(
    std::vector<ModPoint> &mods, const std::vector<MacroRec> &export_macros,
    const std::set<std::string> &block_defined_names,
    const std::vector<std::pair<unsigned, unsigned>> &block_ranges,
    const std::string &original) {
  std::vector<std::pair<unsigned, unsigned>> ranges;
  for (auto &m : export_macros) {
    if (m.name.empty()) continue;  // macro-only conditional block
    if (m.start_off == 0 && m.end_off == 0) continue;  // no source offsets
    if (block_defined_names.count(m.name)) continue;   // kept in its block
    // A definition inside a conditional block stays in the header body with
    // its block. Taking it out on its own would leave the `#undef` that
    // precedes it — the `#undef X` + redefine idiom writes one — with nothing
    // to retract and no definition after it, so the macro ends up undefined
    // for everyone who includes the header.
    if (inside_block(block_ranges, m)) continue;
    ranges.push_back({m.start_off, m.end_off});
    auto guard = override_guard_with_code(original, m.start_off, m.name);
    if (guard.active) {
      // Strip the `#if !defined(X)` guard line, the define, and the matching
      // `#endif`, keeping the inline code in between.
      mods.push_back({guard.guard_start, m.end_off - guard.guard_start, ""});
      auto enl = original.find('\n', guard.endif_start);
      unsigned endif_end = enl == std::string::npos ? (unsigned)original.size()
                                                    : (unsigned)enl + 1;
      mods.push_back({guard.endif_start, endif_end - guard.endif_start, ""});
    } else {
      // Strip the define (and its doc comment, moved to the macros file).
      unsigned begin = m.start_off;
      auto cstart = preceding_comment_start(original, m.start_off);
      if (cstart != 0 && cstart < m.start_off) begin = cstart;
      mods.push_back({begin, m.end_off - begin, ""});
    }
  }
  return ranges;
}

// Split the GMF includes: unguarded system/standard includes come FIRST so
// that feature-test macros (e.g. `__cpp_lib_three_way_comparison` from
// `<compare>`/`<version>`) are defined before the macros file is processed —
// the macros file evaluates them (e.g. `LIB_INTERNAL_HAS_COMPARE_LIB`) to
// decide which entities are emitted. Guarded includes follow the macros file,
// since their guard conditions reference macros defined by the macros file
// itself.
std::pair<std::vector<IncludeDirective>, std::vector<IncludeDirective>>
split_gmf_pre_post(std::vector<IncludeDirective> gmf_incs) {
  std::vector<IncludeDirective> pre, post;
  for (auto &inc : gmf_incs) {
    // `<compare>` and `<version>` define `__cpp_lib_three_way_comparison`,
    // which the macros file evaluates (e.g. `LIB_INTERNAL_HAS_COMPARE_LIB`).
    // They must come BEFORE the macros file even when guarded in the source —
    // their guard macro's VALUE depends on the feature-test macro they define.
    // Their guards reference macros only the macros file provides, so the
    // guards must be stripped or the include would be dead in the GMF. Only
    // the top-level headers qualify: libc++ internal headers like
    // `<__cxx03/version>` must keep their guards. Other feature-test headers
    // can be guarded by macros-file macros (`LIB_INTERNAL_HAS_INCLUDE`), so
    // they must stay AFTER the macros file.
    auto fname = std::filesystem::path(inc.path).filename();
    bool top_level_feature_test =
        !inc.path.contains('/') &&
        (fname == "compare" || fname == "version");
    if (inc.guard_stack.empty() || top_level_feature_test) {
      if (top_level_feature_test) inc.guard_stack.clear();
      pre.push_back(inc);
    } else {
      post.push_back(inc);
    }
  }
  return {std::move(pre), std::move(post)};
}

// Assemble the module interface unit (.cc) from the classified GMF includes,
// purview imports, injected forward declarations and the rewritten header body.
std::string assemble_interface_cc(
    llvm::StringRef module_name, const std::string &header_comment,
    const std::string &use_modules_macro, const std::string &std_import_guard,
    const std::string &std_use_guard, const std::string &macros_name,
    bool has_macros, const RewriteOptions &options,
    const std::vector<IncludeDirective> &gmf_incs,
    const std::vector<std::string> &purview_imports,
    const std::vector<std::pair<std::vector<std::string>, std::string>>
        &fwd_decls,
    const std::string &hdr, llvm::StringRef header_path,
    const std::vector<std::string> &redefined_macros = {},
    const std::string &export_include = {}) {
  std::string cc;
  if (!header_comment.empty()) cc += header_comment + "\n\n";
  cc += "module;\n";

  auto [pre_macros, post_macros] = split_gmf_pre_post(gmf_incs);
  emit_gmf_blocks(cc, pre_macros);

  // The interface unit's GMF includes the macros file so that guard conditions
  // on the GMF includes (e.g. `#ifdef HAS_FEATURE_X`) resolve. After the GMF,
  // the header's own macros are `#undef`'d so the header's `#if !defined(X)`
  // guard blocks re-evaluate as in the original header (the header keeps its
  // macro definitions and re-establishes them).
  if (has_macros) {
    // A macros file whose bodies carry export markers chain-includes the
    // library's export header (`<LIB>-export.h`), which defines `LIB_EXPORT`
    // as `export` only under `LIB_USE_MODULES`. Define it BEFORE the macros
    // file so the module purview (and the GMF) resolves the macro to `export`
    // — otherwise the `#pragma once` header caches the empty definition.
    cc += std::format("#define {} 1\n", use_modules_macro);
    cc += std::format("#include \"{}.h\"\n", macros_name);
  }

  // Guarded GMF includes come after the macros file (their conditions use its
  // macros), merging adjacent identical guards.
  emit_gmf_blocks(cc, post_macros);

  cc += std::format("\nexport module {};\n", module_name.str());
  if (options.import_std) {
    cc += std::format("#ifdef {}\nimport {};\n#endif\n", std_use_guard,
                      std_module_name(options.module_replaces));
  }

  std::set<std::string> seen_imports;
  for (const auto &imp : purview_imports) {
    if (seen_imports.insert(imp).second) cc += imp + "\n";
  }

  // Forward declarations of library entities this module uses before they are
  // visible through an import (see FwdDeclRefVisitor). Emitted in the module
  // purview so the module's own code can reference them; exported only when
  // this module also defines the entity. With extern "C++" the declarations
  // land in the global module fragment, so they — and the included header body
  // below — must be inside the extern "C++" block to merge with the
  // definitions emitted there.
  if (options.extern_cxx) {
    // The injected declarations below carry the marker whose expansion depends
    // on this same wrapping, and the header that defines it is included only
    // further down. Bring the definitions in first — the export header is
    // nothing but #defines.
    if (!export_include.empty()) cc += export_include;
    if (!options.extern_cxx_macro.empty())
      cc += std::format("#ifdef {}\n", options.extern_cxx_macro);
    cc += "extern \"C++\" {\n";
    if (!options.extern_cxx_macro.empty()) cc += "#endif\n";
  }
  // Injected declarations are emitted in dependency order; merge adjacent ones
  // that live in the SAME namespace path into a single `namespace ... { ... }`
  // block instead of one block per declaration. Only adjacent blocks are
  // merged so the topological order (a declaration must come before another
  // that references it) is preserved.
  {
    auto same_path = [](const std::vector<std::string> &a,
                        const std::vector<std::string> &b) {
      if (a.size() != b.size()) return false;
      for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
      return true;
    };
    for (std::size_t i = 0; i < fwd_decls.size();) {
      auto &[nss, decl] = fwd_decls[i];
      for (std::size_t n = 0; n < nss.size(); ++n)
        cc += std::string("namespace ") + nss[n] + " {\n";
      std::set<std::string> seen_decls;
      cc += decl;
      seen_decls.insert(decl);
      std::size_t end = i + 1;
      while (end < fwd_decls.size() && same_path(nss, fwd_decls[end].first)) {
        if (seen_decls.insert(fwd_decls[end].second).second)
          cc += fwd_decls[end].second;
        ++end;
      }
      for (std::size_t n = 0; n < nss.size(); ++n) cc += "}\n";
      i = end;
    }
  }

  cc += std::format("#define {} 1\n", use_modules_macro);
  if (options.cc_only) {
    // Inline the rewritten header body directly into the interface unit so no
    // `.h` is emitted. `USE_MODULES` is defined here, so the body's
    // `#ifndef USE_MODULES` include guards are inactive: every includable
    // header that can be replaced with an import is gone (library headers are
    // `import`ed above; system headers live in the GMF). The only include that
    // remains is the generated export-macro header.
    std::string_view body(hdr);
    if (body.starts_with("#pragma once\n")) body.remove_prefix(13);
    cc += std::string(body);
  } else {
    // Macros the macros file defines AND the header defines again for itself.
    // The macros file is included at the top of the global module fragment, so
    // that guard conditions on the GMF includes resolve — but that is before
    // the standard library has been included, so anything derived from a
    // feature-test macro (`#if defined(__cpp_lib_three_way_comparison)`) is
    // computed there against the wrong answer. The header's own definition,
    // reached after those includes, is the correct one; drop the early copy so
    // it wins outright instead of redefining it.
    for (auto &name : redefined_macros)
      cc += std::format("#undef {}\n", name);
    // The form a consumer writes, not the bare filename: the rewritten header
    // is installed under the include prefix its original had
    // (`gtest/gtest.h`), which is not the directory the interface unit ends up
    // in, so a filename-only include does not resolve. Matches what the
    // generated macro-header include already emits. With no include directory
    // to be relative to, include_form hands back the path as given — fall back
    // to the filename there rather than baking in an absolute path.
    auto form = include_form(header_path, options.extra_args);
    if (form == header_path.str())
      form = std::filesystem::path(header_path.str()).filename().string();
    cc += std::format("#include \"{}\"\n", form);
  }

  if (options.extern_cxx) {
    if (!options.extern_cxx_macro.empty())
      cc += std::format("#ifdef {}\n", options.extern_cxx_macro);
    cc += "}  // extern \"C++\"\n";
    if (!options.extern_cxx_macro.empty()) cc += "#endif\n";
  }
  return cc;
}

}  // namespace

// Which of `macros` the rewritten header still defines FOR ITSELF, so that the
// macros file's early copy has to be dropped before the header is included
// (see assemble_interface_cc).
//
// Keyed on each macro's definition as written, never on its name alone. A macro
// defined in one branch of a conditional and again in the other
//
//   #ifdef __has_include
//   #define HAS_INCLUDE __has_include
//   #else
//   #define HAS_INCLUDE(...) 0
//   #endif
//
// leaves the branch that was NOT taken behind in the header once the taken one
// is hoisted into the macros file. Reading that unreachable definition as a
// redefinition would `#undef` the macros file's copy with nothing left to
// restore it, and every use in the header fails to compile with "function-like
// macro is not defined".
export std::vector<std::string> macros_redefined_by_header(
    const std::string &hdr, const std::vector<MacroRec> &macros) {
  std::vector<std::string> out;
  for (auto &m : macros) {
    if (m.name.empty()) continue;
    std::string_view def(m.body);
    while (!def.empty() && (def.back() == '\n' || def.back() == '\r'))
      def.remove_suffix(1);
    if (def.empty()) continue;
    for (std::size_t pos = hdr.find(def); pos != std::string::npos;
         pos = hdr.find(def, pos + 1)) {
      // At the start of a line, so a definition quoted inside another macro's
      // body does not count as the header defining it.
      auto ls = hdr.rfind('\n', pos);
      auto first =
          hdr.find_first_not_of(" \t", ls == std::string::npos ? 0 : ls + 1);
      if (first != pos) continue;
      out.push_back(m.name);
      break;
    }
  }
  return out;
}

export HeaderRewriteResult rewrite_header(
    llvm::StringRef header_path,
    llvm::StringRef module_name,
    const RewriteOptions &options = {}) { HeaderRewriteResult result;
  result.module_name = module_name.str();

  auto original = read_file(header_path);
  if (original.empty()) {
    llvm::errs() << "error: cannot read " << header_path << "\n";
    return result;
  }

  auto dot = module_name.find('.');
  auto library_name = dot == llvm::StringRef::npos
      ? module_name
      : module_name.substr(0, dot);
  // The LIB prefix of LIB_USE_MODULES / LIB_IMPORT_STD / LIB_USE_IMPORT_STD /
  // LIB_EXPORT defaults to the uppercased library name but can differ from the
  // main module name; an explicit override (--macro-prefix) wins.
  auto prefix = options.macro_prefix_override.empty()
      ? macro_prefix(library_name)
      : macro_prefix(options.macro_prefix_override);
  auto use_modules_macro = prefix + "_USE_MODULES";
  // Guards for the optional import std feature. Building with
  // <LIB>_IMPORT_STD defined skips the guarded stdlib includes; building
  // with <LIB>_USE_IMPORT_STD defined activates `import std;`.
  auto std_import_guard = prefix + "_IMPORT_STD";
  auto std_use_guard = prefix + "_USE_IMPORT_STD";
  auto export_macro = prefix + "_EXPORT";
  // A second marker, for a declaration of an entity that another module
  // defines. Whether it is exported depends on how the tree is BUILT, not on
  // how it was generated: wrapped, the entity is attached to the global module
  // and importers have to find this declaration to merge with it; unwrapped, it
  // stays attached to its own module, where exporting it from here would
  // declare a second entity of the same name in a second module.
  // A declaration of an entity another module defines is exported either way —
  // that is what lets an importer merge with it instead of declaring its own —
  // and unwrapped it also needs the `extern "C++"` the wrapping would otherwise
  // have given it, or it attaches to the module declaring it and stops being
  // the same entity as the definition.
  auto shared_linkage_macro = prefix + "_EXTERN_CXX_DECL";
  // Stands where such a declaration's own `extern` keyword was. Wrapped, the
  // block's braces mean the declaration is not DIRECTLY contained in the
  // linkage-specification, so [dcl.link]/7 does not supply the keyword and it
  // has to keep it; unwrapped, `extern "C++"` contains it directly and does.
  // It stays in the keyword's place rather than joining the marker at the
  // front, because an attribute may sit between the two and a standard
  // attribute in the middle of the decl-specifiers is ill-formed.
  auto extern_decl_macro = prefix + "_EXTERN_DECL";
  auto shared_marker = std::format("{} {}", export_macro, shared_linkage_macro);

  auto includes = parse_includes(original);
  // Transitive includes: system headers go to the GMF; quoted library
  // headers become module imports. A dependant that reaches an internal
  // module indirectly must still import it directly, so we expand the
  // full include closure (cycle-safe). Quoted library headers and system
  // headers resolvable via the include path are expanded; stdlib headers
  // are not. Transitive system includes are only kept in the GMF if the
  // module actually uses them (see used_headers filtering below).
  expand_include_closure(includes, header_path, options.extra_args);

  // Annotate includes with their #ifdef guard context.
  annotate_guards(original, includes);

  std::regex internal_re(kDefaultInternalFilter.str());
  std::vector<ModPoint> mods;
  std::vector<MacroRec> macros;

  std::vector<std::string> sources = {header_path.str()};
  auto flags = base_compile_flags();
  flags.insert(flags.end(), options.extra_args.begin(), options.extra_args.end());
  clang::tooling::FixedCompilationDatabase db(".", flags);
  clang::tooling::ClangTool tool(db, sources);
  std::set<std::string> used_headers;
  std::vector<std::pair<std::vector<std::string>, std::string>> fwd_decls;
  VisitorFrontendActionFactory factory(
      [&](clang::CompilerInstance &ci) {
        ci.getPreprocessor().addPPCallbacks(
            std::make_unique<HeaderMacroCollector>(ci.getSourceManager(),
                                                   macros));
        return std::make_unique<HeaderRewriteConsumer>(
            ci.getASTContext(), mods, internal_re, export_macro,
            shared_marker, shared_linkage_macro, extern_decl_macro,
            options.reachable_fqns, result.internal_fqns, options.no_internal_filter,
            &used_headers, options.defined_fqns, &fwd_decls, options.extern_cxx,
            options.extern_cxx_macro, options.fwd_declared_fqns, options.same_module_free_fqns,
            options.library_headers.empty() ? nullptr : &options.library_headers,
            &result.external_macro_mods);
      });
  tool.run(&factory);

  // Merge export markers routed from OTHER library headers (e.g. a macro
  // defined in matchers.h but invoked in more-matchers.h)
  // into this header's macro-body markers so build_macros_file bakes them into
  // the macro definition wherever it expands.
  for (auto &m : options.extra_macro_mods) mods.push_back(m);

  // Textually extract ALL #define lines from the original header,
  // including those inside inactive #if blocks. These are needed so that
  // guard macros in the GMF resolve correctly on any platform.
  extract_textual_macros(original, macros);

  // A macro whose name is (re)defined inside an emitted conditional block must
  // not also be emitted as a bare unconditional #define from the active set:
  // the guarded block is authoritative on every platform, and emitting both
  // causes "macro redefined" diagnostics. Collect the names the blocks cover.
  auto block_defined_names = collect_block_defined_names(macros);

  auto stem = std::filesystem::path(header_path.str()).stem().string();

  auto block_ranges = collect_block_ranges(macros);
  std::vector<MacroRec> export_macros, impl_macros;
  for (auto &m : macros) {
    if (m.undefed)
      impl_macros.push_back(std::move(m));
    else
      export_macros.push_back(std::move(m));
  }

  // PUBLIC macros (never #undef'd before the end of the header) move to the
  // generated macros file and are stripped from the header body; PRIVATE macros
  // (undef'd before the end) stay in the body, exactly as in the original. A
  // public macro defined inside a macro-only conditional block is kept in the
  // body too — the block also carries include directives the body needs — and
  // the macros file re-provides it (identical redefinition is legal). A public
  // macro inside an `#if !defined(X)` guard that gates inline code (a
  // `#if !defined(LOG_)` guard) must have the guard stripped along with the
  // define, otherwise the guard re-evaluates with the macro already defined and
  // the inline helpers are lost.
  // Ranges of macro definitions that move to the macros file. The visitor
  // marks entities defined INSIDE a macro body at their spelling location (so
  // the marker travels wherever the macro expands); those insertions belong to
  // the macros file (applied by build_macros_file), never to the header body,
  // where the macro definition is stripped out entirely.
  auto stripped_macro_ranges = collect_stripped_ranges(
      mods, export_macros, block_defined_names, block_ranges, original);

  // The header body keeps the raw macro invocation; the export markers the
  // visitor placed INSIDE a macro definition (spelling locations) belong to the
  // macros file only. Drop them here so the stripped macro region isn't
  // re-corrupted by stray insertions, then apply the remaining mods.
  std::vector<ModPoint> header_mods;
  for (auto &m : mods) {
    if (m.len == 0 && std::ranges::any_of(stripped_macro_ranges,
                                          [&](const auto &r) {
                                            return m.offset >= r.first &&
                                                   m.offset < r.second;
                                          }))
      continue;
    header_mods.push_back(m);
  }
  std::string hdr = apply_mods(original, header_mods);

  // Wrap include directives in hdr with #ifndef USE_MODULES.
  // Skip includes that are inside #ifdef blocks (they keep original guards).
  // In cc-only mode the header body is inlined into the interface unit where
  // USE_MODULES is always defined, so these includes are dropped entirely.
  hdr = wrap_includes_with_guard(std::move(hdr), includes, use_modules_macro,
                                 /*remove=*/options.cc_only);

  auto prelude = std::format(
      "#pragma once\n"
      "#ifdef {0}\n"
      "#define {1} export\n"
      "#else\n"
      "#define {1}\n"
      "#endif\n",
      use_modules_macro, export_macro);
  // The wrapping gives every entity in the purview C++ language linkage in the
  // global module. Where it is absent, a declaration of an entity another
  // module defines has to ask for that linkage itself.
  if (!options.extern_cxx) {
    prelude += std::format("#define {} extern \"C++\"\n", shared_linkage_macro);
    prelude += std::format("#define {}\n", extern_decl_macro);
  } else if (options.extern_cxx_macro.empty()) {
    prelude += std::format("#define {}\n", shared_linkage_macro);
    prelude += std::format("#define {} extern\n", extern_decl_macro);
  } else {
    prelude += std::format(
        "#ifdef {0}\n#define {1}\n#define {2} extern\n"
        "#else\n#define {1} extern \"C++\"\n#define {2}\n#endif\n",
        options.extern_cxx_macro, shared_linkage_macro, extern_decl_macro);
  }

  // The export header is shared by every header of the library. It lives at
  // the library's include root (`mylib/mylib-export.h`) when the library uses
  // an include subdirectory, otherwise at the top level (`test_lib_export.h`).
  // Named with the same hyphen/underscore style as the macro headers.
  auto inc_prefix = include_prefix(header_path, library_name);
  auto export_sep = options.hyphen_macros ? "-" : "_";
  auto exp_dir = inc_prefix.empty() ? "" : std::format("{}/", library_name.str());
  auto export_h_name = exp_dir + std::format("{}{}export",
      library_name.str(), export_sep);
  auto export_include = std::format("#include \"{}.h\"\n", export_h_name);

  std::size_t insert_pos = 0;
  if (hdr.starts_with("#pragma once")) {
    auto nl = hdr.find('\n');
    if (nl != std::string::npos) insert_pos = nl + 1;
  } else if (hdr.starts_with("#ifndef")) {
    auto first = hdr.find('\n');
    if (first != std::string::npos) {
      auto second = hdr.find('\n', first + 1);
      if (second != std::string::npos &&
          std::string_view(hdr).substr(first + 1, second - first - 1)
              .starts_with("#define"))
        insert_pos = second + 1;
      else
        insert_pos = first + 1;
    }
  }
  hdr.insert(insert_pos, export_include);
  auto post_guard_pos = insert_pos + export_include.size();
  result.export_h_content = std::move(prelude);
  result.export_h_name = std::move(export_h_name);

  // Compute auto-imports before generating macros content so we can
  // chain-include dependency macros files.
  std::set<std::string> extra_macro_includes;
  auto auto_imports = compute_auto_imports(
      includes, library_name, module_name, header_path, options.extra_args,
      result.imported_modules, extra_macro_includes,
      options.hyphen_macros,
      options.library_headers.empty() ? nullptr : &options.library_headers);

  // The macros file lives next to the header that uses it (`mylib/` for a
  // public header, `mylib/internal/` for an internal one) so the deployed
  // layout mirrors the original headers.
  auto macros_name = inc_prefix + macro_base_name(stem, options.hyphen_macros) +
                     (options.hyphen_macros ? "-" : "_") + "macros";

  // Modules this module imports; used to drop includes that an imported
  // module provides (options.module_replaces).
  std::set<std::string> imported_modules;
  for (auto &[p, m] : options.include_to_module) imported_modules.insert(m);
  for (auto &[p, m] : auto_imports) imported_modules.insert(m);
  if (options.import_std) {
    imported_modules.insert("std");
    imported_modules.insert("std.compat");
  }

  if (!export_macros.empty() || !extra_macro_includes.empty()) {
    result.macros_content = build_macros_file(
        export_macros, extra_macro_includes, block_defined_names, block_ranges,
        original, mods, export_include);
    result.macros_name = macros_name;
    // The macro definitions moved out of the header into the macros file, and
    // the body still uses them (`GTEST_INTERNAL_HAS_INCLUDE(<span>)`). The
    // interface unit supplies them from its global module fragment, but a
    // classic (non-module) build includes this header and nothing else, so the
    // header has to bring its own macros along in that case.
    //
    // Whatever the header goes on to define for itself has to be dropped again
    // straight after that include, for the same reason the interface unit
    // drops it (see assemble_interface_cc): the macros file is written before
    // the standard library is, so a macro derived from a feature-test macro is
    // computed there against the wrong answer —
    //
    //   #if defined(__cpp_lib_three_way_comparison)
    //   #define LIB_HAS_COMPARE 1
    //
    // reaches the macros file as 0 and the header as 1. The header's is the
    // right one and wins by coming later, so the value was never wrong here,
    // but every translation unit including the header said so:
    //
    //   warning: 'LIB_HAS_COMPARE' macro redefined
    //
    // Undefining first leaves the header to establish its own definitions
    // quietly, and leaves the macros file to cover only what the header no
    // longer defines — which is what it is for.
    // Drop the macros file's copy of each macro the header defines for itself,
    // immediately before that definition rather than in one block up front.
    //
    // The values are the reason. The macros file is written before the
    // standard library is, so a macro derived from a feature-test macro is
    // computed there against the wrong answer, and the header's own definition
    // is the one to keep — but until that definition is reached, the macros
    // file's value is what the header's own conditionals read:
    //
    //   #ifdef LIB_IS_THREADSAFE      // decided by a macro the file supplied
    //   ...
    //   #define LIB_STATIC_MUTEX_(m)  // and only redefined inside a branch
    //
    // Undefining up front takes those away before they are read, silently
    // selecting a different branch and so a different definition. Undefining
    // at the point of redefinition leaves every read intact and drops the copy
    // exactly when it is about to be replaced.
    // EVERY definition of the name, not only the one that is live here. A
    // header often sets a default and then replaces it under a condition:
    //
    //   #define LIB_WARNING_PUSH()              // default, collides
    //   #if defined(__clang__)
    //   #undef LIB_WARNING_PUSH
    //   #define LIB_WARNING_PUSH() _Pragma(...) // live, what the file carries
    //
    // The macros file carries the live one, so it is the default that
    // redefines it, and an undef placed only at the live definition arrives
    // after the warning has already been issued.
    std::vector<std::pair<std::size_t, std::string>> sites;
    for (auto &m : export_macros) {
      if (m.name.empty()) continue;
      auto needle = std::format("#define {}", m.name);
      for (std::size_t pos = hdr.find(needle); pos != std::string::npos;
           pos = hdr.find(needle, pos + 1)) {
        // This very macro, not one whose name merely starts with it, and at
        // the start of a line.
        auto after = pos + needle.size();
        if (after < hdr.size() &&
            (is_ident_char(hdr[after]) || hdr[after] == '_'))
          continue;
        auto ls = hdr.rfind('\n', pos);
        auto first =
            hdr.find_first_not_of(" \t", ls == std::string::npos ? 0 : ls + 1);
        if (first != pos) continue;
        sites.emplace_back(pos, std::format("#undef {}\n", m.name));
      }
    }
    // The guarded include of this header's own macros file belongs to the same
    // pass. Its offset was measured against the text as it stands now, and so
    // were the `#undef` offsets above; applying it afterwards would use a
    // position every `#undef` inserted ahead of it has already moved, landing
    // that many characters short — inside whatever token now sits there:
    //
    //   #define LIB_HPP_INC#ifndef LIB_USE_MODULES
    //   #include "lib_macros.h"
    //   #endif
    //   LUDED
    //
    // which is silent: the header still has a `#define`, and the name it
    // defines is simply the wrong one.
    sites.emplace_back(post_guard_pos,
                       std::format("#ifndef {}\n#include \"{}.h\"\n#endif\n",
                                   use_modules_macro, macros_name));
    // Back to front, so the offsets of the sites still to come do not shift.
    std::ranges::sort(sites, [](auto &a, auto &b) { return a.first > b.first; });
    for (auto &[pos, text] : sites) hdr.insert(pos, text);
  }

  // Reproduce the source file's license/copyright header at the very top of the
  // generated module (before the global module fragment) and of the rewritten
  // header, so attribution survives.
  auto header_comment = extract_file_header_comment(original);

  result.h_content = hdr;
  if (!header_comment.empty())
    result.h_content = header_comment + "\n\n" + result.h_content;

  // Build .cc content:
  // module;
  // #include <system_headers>  (in GMF)
  // export module X;
  // import library_dep;        (in purview)
  // #define USE_MODULES 1
  // #include "rewritten_header.h"

  // Classify includes into GMF includes (system/third-party) vs purview
  // imports (library headers replaced by `import`).
  // A declaration the module uses may be declared in a standard-library
  // private header; that header is never emitted, so credit the use to the
  // public header that owns it (see used_public_ancestors).
  for (auto &a : used_public_ancestors(includes, header_path, options.extra_args,
                                       used_headers))
    used_headers.insert(a);
  auto [gmf_incs, purview_imports] = classify_includes(
      includes, auto_imports, library_name, module_name, header_path, options,
      imported_modules, used_headers, std_import_guard);

  // Drop transitive system includes whose parent header is also emitted in
  // the GMF (e.g. <sys/stat.h> also pulls in <bits/stat.h>; keeping both
  // would redefine struct stat).
  filter_subsumed_transitive(gmf_incs, header_path, options.extra_args);

  // Macros the macros file carries a copy of AND the header still defines for
  // itself: those need dropping before the header is included (see
  // assemble_interface_cc).
  std::vector<std::string> redefined_macros;
  if (!result.macros_content.empty()) {
    redefined_macros = macros_redefined_by_header(hdr, export_macros);
  }

  result.cc_content = assemble_interface_cc(
      module_name, header_comment, use_modules_macro, std_import_guard,
      std_use_guard, macros_name,
      !export_macros.empty() || !extra_macro_includes.empty(), options,
      gmf_incs, purview_imports, fwd_decls, hdr, header_path,
      redefined_macros, export_include);

  return result;
}

export struct SourceRewriteResult {
  std::string content;
  // Dual-mode only: the module implementation unit that wraps `content`.
  // Empty when the source is rewritten into a module unit in place.
  std::string module_content;
  std::string module_name;
  std::vector<std::pair<std::string, std::string>> imported_modules;
};

// Rewrite an implementation file (.cpp/.cc/.cxx) into a module implementation
// unit. Library-internal includes become imports; system/third-party includes
// move into the global module fragment; x-macro includes stay in the body.
// Unlike the interface units from rewrite_header, implementation units use a
// bare `module X;` (nothing is exported), so every import is plain and there
// is no export/use-modules macro machinery.
export SourceRewriteResult rewrite_source(
    llvm::StringRef source_path,
    llvm::StringRef module_name,
    const std::map<std::string, std::string> &include_to_module = {},
    const std::vector<std::string> &extra_args = {},
    bool import_std = false,
    bool extern_cxx = false,
    const std::map<std::string, std::set<std::string>> &module_replaces = {},
    const std::set<std::string> &macro_files = {},
    const std::set<std::string> &interface_modules = {},
    const std::vector<std::string> &fwd_declared_fqns = {},
    bool hyphen_macros = false,
    const std::string &umbrella_module = {},
    // Non-empty switches on dual mode: the source keeps its includes (guarded
    // by this macro) so it still compiles as a plain translation unit, and the
    // module implementation unit is emitted separately in `module_content`.
    const std::string &use_modules_macro = {},
    // Path of the body file as seen from the module unit (defaults to the
    // CLI layout: `impl/<stem>.cc` included from `impl/modules/<stem>.cc`).
    const std::string &body_include = {},
    // When set, the extern "C++" wrapping is written behind `#ifdef <this>`,
    // matching what the interface units do. The two have to agree: an
    // interface with module linkage and a body with C++ language linkage
    // declare and define different entities.
    const std::string &extern_cxx_macro = {},
    // Headers this run is rewriting, canonicalized. An include resolving into
    // it is one of ours and becomes an import whatever its spelling.
    const std::set<std::string> &library_headers = {}) {

  SourceRewriteResult result;
  result.module_name = module_name.str();

  auto original = read_file(source_path);
  if (original.empty()) {
    llvm::errs() << "error: cannot read " << source_path << "\n";
    return result;
  }

  auto dot = module_name.find('.');
  auto library_name = dot == llvm::StringRef::npos
      ? module_name
      : module_name.substr(0, dot);

  auto fs_stem = std::filesystem::path(source_path.str()).stem().string();

  auto includes = parse_includes(original);
  // Expand the include closure so system headers the implementation code
  // reaches transitively through (now imported) library headers can be kept in
  // the GMF. Kept only when the code actually references them (used_headers).
  expand_include_closure(includes, source_path, extra_args);

  annotate_guards(original, includes);

  // Run the implementation file to find which system headers its code
  // references, so transitive system includes can be filtered to the used set.
  std::vector<std::string> sources = {source_path.str()};
  auto flags = base_compile_flags();
  flags.insert(flags.end(), extra_args.begin(), extra_args.end());
  clang::tooling::FixedCompilationDatabase db(".", flags);
  clang::tooling::ClangTool tool(db, sources);
  std::set<std::string> used_headers;
  VisitorFrontendActionFactory used_factory(
      [&used_headers](clang::CompilerInstance &) {
        return make_traverse_consumer<UsedHeadersVisitor>(used_headers);
      });
  // A failed parse yields no AST, so `used_headers` stays empty and every
  // transitive system include is filtered out of the global module fragment —
  // the unit then fails to compile on declarations it demonstrably uses
  // (`getcwd`, `mkdir`, ...). That is silent otherwise: say so.
  if (tool.run(&used_factory) != 0)
    llvm::errs() << "warning: " << source_path
                 << " did not parse cleanly; transitive system includes cannot "
                    "be resolved and may be missing from the global module "
                    "fragment\n";

  // See rewrite_header: a use inside a standard-library private header counts
  // as a use of the public header that owns it.
  for (auto &a : used_public_ancestors(includes, source_path, extra_args,
                                       used_headers))
    used_headers.insert(a);

  // Quoted `{library}/...` includes auto-derive their module name, mirroring
  // rewrite_header's auto-import behavior.
  std::vector<std::pair<std::string, std::string>> imported;
  std::set<std::string> extra_macro_includes;
  auto auto_imports = compute_auto_imports(
      includes, library_name, module_name, source_path, extra_args, imported,
      extra_macro_includes, hyphen_macros,
      library_headers.empty() ? nullptr : &library_headers);
  result.imported_modules = std::move(imported);

  std::vector<IncludeDirective> gmf_incs;
  std::vector<std::string> imports;
  std::vector<ModPoint> deletions;

  // An include the module unit no longer needs (it became an import, moved to
  // the global module fragment, or is provided by an imported module). In
  // module-unit mode it is deleted from the body; in dual mode the body stays
  // compilable on its own, so the directive is kept and guarded instead.
  // Marks the body as being compiled inside the module unit rather than on its
  // own. Derived from the use-modules macro so the two read as a pair.
  std::string module_unit_macro = use_modules_macro;
  if (module_unit_macro.ends_with("_USE_MODULES"))
    module_unit_macro =
        module_unit_macro.substr(0, module_unit_macro.size() -
                                        std::strlen("_USE_MODULES")) +
        "_MODULE_UNIT";
  else
    module_unit_macro += "_MODULE_UNIT";

  bool dual = !use_modules_macro.empty();
  std::vector<std::pair<unsigned, unsigned>> guarded_spans;
  // Includes the body still needs when it is compiled as an ordinary
  // translation unit rather than inside the module unit. A dropped include is
  // safe to lose only because something else provides what it declared: an
  // import for a library header, `import std` for a standard one. With no
  // `import std`, nothing replaces a standard header, and the body compiled on
  // its own is left without it — so those are guarded against the module unit
  // instead, which does provide them from its global module fragment.
  std::vector<std::pair<unsigned, unsigned>> body_needed_spans;
  auto drop_include = [&](const IncludeDirective &inc) {
    if (!dual) {
      deletions.push_back({inc.offset, inc.end_offset - inc.offset});
      return;
    }
    if (!import_std && !inc.is_quoted)
      body_needed_spans.push_back({inc.offset, inc.end_offset});
    else
      guarded_spans.push_back({inc.offset, inc.end_offset});
  };

  // Collect member/entity definitions that must be `extern "C++"` because the
  // interface declares them that way (cross-module forward-declared entities).
  // Also run when the source defines `main` (which may not be a module entity).
  bool has_main = original.contains("main(");
  // Also when the tree is generated WITH the wrapping: the body still has to
  // say what linkage its definitions have, because the wrapping is chosen when
  // the tree is BUILT. Built wrapped, the marker expands to nothing and the
  // purview block covers everything; built unwrapped, it is the `extern "C++"`
  // that keeps a definition the same entity as the declaration it matches.
  if (!fwd_declared_fqns.empty() || has_main) {
    std::set<std::string> fwd_set(fwd_declared_fqns.begin(),
                                  fwd_declared_fqns.end());
    std::string marker = "extern \"C++\" ";
    std::string wrap_guard;
    if (extern_cxx) {
      if (extern_cxx_macro.empty()) return result;  // always wrapped: no marker
      auto prefix = extern_cxx_macro.substr(
          0, extern_cxx_macro.size() - std::strlen("_EXTERN_CXX"));
      marker = prefix + "_EXTERN_CXX_DECL ";
      wrap_guard = extern_cxx_macro;
    }
    clang::tooling::ClangTool extern_tool(db, sources);
    VisitorFrontendActionFactory extern_factory(
        [&](clang::CompilerInstance &ci) {
          return std::make_unique<ExternCxxDefsConsumer>(
              ci.getASTContext(), fwd_set, deletions, marker, wrap_guard);
        });
    extern_tool.run(&extern_factory);
  }
  std::set<std::string> imported_mods;
  // With `import std;` the std module replaces the C++ standard-library
  // headers: mark it (and std.compat) as imported so classify_replacement
  // strips the C++ stdlib includes and the libc++/libstdc++ internal
  // __-prefixed headers from the global module fragment (they are not to be
  // included next to `import std;`).
  if (import_std) {
    imported_mods.insert("std");
    imported_mods.insert("std.compat");
  }
  for (auto &inc : includes) {
    if (is_xmacro_include(inc.path)) continue;  // repeatable include stays

    // Transitive includes (from expanded dependency headers) do not exist in
    // this source body: they are never deleted. Only used transitive system
    // headers are emitted in the GMF.
    if (inc.transitive) {
      // See classify_includes: standard-library private headers are never
      // emitted, only the public header that owns them.
      if (inc.system_internal) continue;
      if (!inc.is_quoted) {
        auto resolved = resolve_include(inc.path, source_path.str(), extra_args);
        if (!keep_transitive_system_include(resolved, inc.path,
                                            used_headers)) {
          // `<version>` defines only preprocessor macros the impl body
          // evaluates (e.g. `__cpp_lib_char8_t`); macros are not tracked by
          // used_headers, but the header must still be present for the code to
          // compile (e.g. a `#ifdef __cpp_lib_char8_t` definition in the impl).
          continue;
        }
        if (!resolved.empty()) {
          // Headers provided by the imported std module (C++ stdlib headers and
          // libc++/libstdc++ internal __-prefixed headers) are stripped: they
          // must not be included in the GMF next to `import std;`.
          auto rc = classify_replacement(inc, source_path, extra_args,
                                         imported_mods, module_replaces);
          if (rc.other_replaced || rc.std_replaced) continue;
          if (inc.skip_gmf) {
            // The include sits in an `#elif` branch (e.g. `<regex.h>` behind
            // `#elif LIB_HAS_POSIX_RE`). `#elif` text cannot stand alone in
            // the GMF, but the impl body demonstrably uses this header, so the
            // branch was active: emit it unconditionally.
            inc.guard_stack.clear();
          }
          gmf_incs.push_back(inc);
        }
      } else if (!inc.path.contains("/custom/")) {
        // A transitive quoted library include. The impl body may reference
        // entities from it directly even though its own header does not import
        // the module (the umbrella does not re-export internal modules), so
        // import it when it is actually used.
        auto derived = derive_module_name(inc.path, library_name);
        auto lib_prefix = std::format("{}/", library_name.str());
        if (inc.path.starts_with(lib_prefix) ||
            derived == library_name.str()) {
          if (!umbrella_module.empty() && derived == library_name.str())
            derived = umbrella_module;
          if (derived != module_name.str() && !imported_mods.count(derived)) {
            auto resolved =
                resolve_include(inc.path, source_path.str(), extra_args);
            if (!resolved.empty() && used_headers.count(resolved)) {
              imports.push_back(derived);
              imported_mods.insert(derived);
            }
          }
        }
      }
      continue;
    }

    // A header explicitly provided by a module (`--module-replaces`): import
    // that module instead of keeping the include (same as rewrite_header).
    // Prefer the most specific replacing module (fewest headers).
    std::string replaced_mod =
        find_replacing_module(inc.path, module_replaces);
    if (!replaced_mod.empty() && replaced_mod != "std" &&
        replaced_mod != "std.compat") {
      if (!imported_mods.count(replaced_mod)) {
        imports.push_back(replaced_mod);
        imported_mods.insert(replaced_mod);
      }
      drop_include(inc);
      continue;
    }

    // Resolve the module for a library include: explicit map, auto-imports, or
    // derived from the `{library}/` path prefix. compute_auto_imports skips
    // the library's umbrella module (to avoid header-interface cycles), but an
    // implementation unit may import it, so it is resolved here as well.
    // A quoted include of a same-library header under a source directory
    // (e.g. `src/mylib-internal-inl.h`) is auto-imported under its first path
    // segment (`src.mylib_internal_inl`), which is wrong: the interface module
    // is derived from the LIBRARY name (`mylib.mylib_internal_inl`). Prefer
    // the library-derived module name when it names a known interface.
    std::string mname;
    bool is_library = false;
    auto it = include_to_module.find(inc.path);
    auto ait = auto_imports.find(inc.path);
    auto lib_derived = inc.is_quoted
        ? derive_module_name(inc.path, library_name)
        : std::string{};
    if (it != include_to_module.end()) {
      mname = it->second;
      is_library = true;
    } else if (inc.is_quoted &&
               !inc.path.contains("/custom/") &&
               !lib_derived.empty() && interface_modules.count(lib_derived)) {
      mname = lib_derived;
      is_library = true;
    } else if (ait != auto_imports.end()) {
      mname = ait->second;
      is_library = true;
    } else if (inc.is_quoted &&
               !inc.path.contains("/custom/")) {
      auto lib_prefix = std::format("{}/", library_name.str());
      if (inc.path.starts_with(lib_prefix) ||
          lib_derived == library_name.str() ||
          interface_modules.count(lib_derived)) {
        mname = lib_derived;
        is_library = true;
      }
    }

    // In wrapper-module mode the umbrella module is renamed (<lib>.umbrella);
    // an impl unit that includes the umbrella header must import the renamed
    // module, not the wrapper facade module that takes the bare library name
    // (importing the wrapper from the umbrella's own impl would be circular).
    bool umbrella_include = is_library && mname == library_name.str();
    if (umbrella_include && !umbrella_module.empty()) mname = umbrella_module;

    // The library's umbrella module exports no macros (macros cannot cross
    // module boundaries), so an impl unit that imports it still needs the
    // umbrella's macro file in the GMF for the macros its code uses.
    if (umbrella_include) {
      auto sep = hyphen_macros ? "-" : "_";
      auto dir = std::filesystem::path(inc.path).parent_path().string();
      extra_macro_includes.insert(
          (dir.empty() ? "" : dir + "/") +
          macro_base_name(std::filesystem::path(inc.path).stem().string(),
                          hyphen_macros) +
          sep + "macros.h");
    }

    // An include that is this very module's interface is implicitly visible in
    // an implementation unit, so it is dropped entirely (not imported, not
    // kept in the GMF).
    if (is_library && mname == module_name.str()) {
      drop_include(inc);
      continue;
    }

    if (!is_library) {
      // System/third-party include: move to the global module fragment.
      if (!inc.skip_gmf) {
        auto rc = classify_replacement(inc, source_path, extra_args,
                                       imported_mods, module_replaces);
        if (rc.other_replaced || rc.std_replaced) {
          drop_include(inc);
          continue;
        }
        gmf_incs.push_back(inc);
      }
      drop_include(inc);
      continue;
    }

    // Conditional library includes cannot become (unconditional) imports,
    // so they are kept as raw GMF includes against the original header.
    if (!inc.guard_stack.empty()) {
      llvm::errs() << "warning: conditional library include '" << inc.path
                   << "' in " << source_path
                   << " cannot be converted to an import; kept in GMF\n";
      gmf_incs.push_back(inc);
      drop_include(inc);
      continue;
    }
    imports.push_back(mname);
    imported_mods.insert(mname);
    drop_include(inc);
  }

  std::vector<std::string> unique_imports;
  {
    std::set<std::string> seen;
    for (auto &m : imports)
      if (seen.insert(m).second) unique_imports.push_back(m);
  }

  // Dual mode: the dropped includes stay in the body, wrapped in
  // `#ifndef LIB_USE_MODULES`, so the file still compiles as a plain
  // translation unit. Directives separated only by whitespace share one guard,
  // so the usual include block at the top of a source gets a single one.
  // One ordered pass over both kinds of span. Merging each kind separately and
  // inserting afterwards interleaves their markers when the two are adjacent —
  // a library include next to a standard one — producing guards that open in
  // one order and close in the other. Adjacent spans merge only when they
  // carry the same guard.
  if (dual) {
    std::vector<std::tuple<unsigned, unsigned, const std::string *>> spans;
    for (auto &[a, b] : guarded_spans)
      spans.emplace_back(a, b, &use_modules_macro);
    for (auto &[a, b] : body_needed_spans)
      spans.emplace_back(a, b, &module_unit_macro);
    std::ranges::sort(spans, [](auto &x, auto &y) {
      return std::get<0>(x) < std::get<0>(y);
    });
    std::vector<std::tuple<unsigned, unsigned, const std::string *>> runs;
    for (auto &sp : spans) {
      auto gap = runs.empty()
          ? std::string::npos
          : original.find_first_not_of(" \t\r\n", std::get<1>(runs.back()));
      if (!runs.empty() && std::get<2>(runs.back()) == std::get<2>(sp) &&
          (gap == std::string::npos || gap >= std::get<0>(sp)))
        std::get<1>(runs.back()) = std::get<1>(sp);
      else
        runs.push_back(sp);
    }
    // One insertion per offset, built in order. Where a run ends exactly where
    // the next begins — an include directly followed by another — two separate
    // insertions at that offset are applied in whichever order the mod list
    // settles on, and the guards come out crossed:
    //
    //   #ifndef A          #ifndef A
    //   #include x         #include x
    //   #endif  // A       #ifndef B      <-- opened before A closed
    //   #ifndef B          #endif  // A
    //
    // Concatenating at the offset keeps the order they were generated in.
    std::map<unsigned, std::string> at;
    for (auto &[from, to, macro] : runs) {
      at[from] += std::format("#ifndef {}\n", *macro);
      at[to] += std::format("#endif  // {}\n", *macro);
    }
    for (auto &[off, text] : at) deletions.push_back({off, 0, text});
  }

  auto body = apply_mods(original, deletions);

  // Drop transitive system includes whose parent header is also emitted in the
  // GMF (e.g. <sys/stat.h> also pulls in <bits/stat.h>; keeping both would
  // redefine declarations).
  filter_subsumed_transitive(gmf_incs, source_path, extra_args);

  std::string out;
  out += "module;\n";
  // Feature-test macros (`__cpp_lib_*`) come from the C++ stdlib headers the
  // source included. When those headers are replaced by `import std.compat;`
  // the macros stop being defined textually, so `#ifdef __cpp_lib_char8_t`
  // blocks in the body compile out and their definitions are lost at link
  // time. `<version>` defines every feature-test macro; emit it before the
  // macro files so both the body's guards and the macro files evaluate
  // correctly.
  out += "#include <version>\n";
  // Macros do not cross module boundaries: an implementation unit needs the
  // macro files of the modules it uses (and its own module's) in its GMF.
  // The umbrella module's macros are included unconditionally (an impl body
  // may use public macros even when it does not import the umbrella), guarded
  // by the caller's `macro_files` so missing macro files cannot break the
  // build.
  auto sep = hyphen_macros ? "-" : "_";
  // The umbrella macro file lives at the library's include root
  // (`mylib/mylib-macros.h`).
  auto umbrella_macros = std::format("{}/{}{}macros.h",
      library_name.str(), library_name.str(), sep);
  if (macro_files.count(umbrella_macros))
    out += std::format("#include \"{}\"\n", umbrella_macros);
  for (auto &em : extra_macro_includes)
    if (macro_files.count(em))
      out += std::format("#include \"{}\"\n", em);
  // This module's own macro file: its include directory is the module path
  // minus the final segment (`mylib/mylib-printers-macros.h`,
  // `mylib/internal/mylib-port-macros.h`).
  std::string own_prefix = library_name.str() + "/";
  auto mod_parts = split_on(module_name.str(), '.');
  for (std::size_t i = 1; i + 1 < mod_parts.size(); ++i)
    own_prefix += macro_base_name(mod_parts[i], hyphen_macros) + "/";
  auto own_macros = own_prefix + std::format("{}{}macros.h", fs_stem, sep);
  if (macro_files.count(own_macros))
    out += std::format("#include \"{}\"\n", own_macros);
  emit_gmf_blocks(out, gmf_incs);
  out += std::format("\nmodule {};\n", module_name.str());
  if (import_std) out += std::format("import {};\n", std_module_name(module_replaces));
  for (auto &m : unique_imports)
    out += std::format("import {};\n", m);

  // Wrapping the body, optionally behind the same macro the interface units
  // use, so the two always agree about which linkage the entities have.
  auto open_extern = [&](std::string &o) {
    if (!extern_cxx) return;
    if (!extern_cxx_macro.empty()) o += std::format("#ifdef {}\n", extern_cxx_macro);
    o += "extern \"C++\" {\n";
    if (!extern_cxx_macro.empty()) o += "#endif\n";
  };
  auto close_extern = [&](std::string &o) {
    if (!extern_cxx) return;
    if (!extern_cxx_macro.empty()) o += std::format("#ifdef {}\n", extern_cxx_macro);
    o += "}  // extern \"C++\"\n";
    if (!extern_cxx_macro.empty()) o += "#endif\n";
  };

  if (dual) {
    // `module;` has to be the first token of the translation unit — clang
    // rejects it even behind an `#ifdef` that is taken — so the two modes
    // cannot share a file. The module implementation unit is a preamble that
    // textually includes the body, which compiles out its own includes because
    // this unit defines the use-modules macro.
    out += std::format("\n#ifndef {}\n#define {} 1\n#endif\n",
                       use_modules_macro, use_modules_macro);
    // Tells the body it is being compiled as part of a module unit, so it
    // leaves the imports below to this preamble.
    out += std::format("#define {} 1\n", module_unit_macro);
    // The body carries these markers on its cross-module definitions, and its
    // own includes are compiled out here, so nothing would define them.
    if (!extern_cxx_macro.empty()) {
      auto pfx = extern_cxx_macro.substr(
          0, extern_cxx_macro.size() - std::strlen("_EXTERN_CXX"));
      out += std::format(
          "#ifndef {1}\n#ifdef {0}\n#define {1}\n#define {2} extern\n"
          "#else\n#define {1} extern \"C++\"\n#define {2}\n"
          "#endif\n#endif\n",
          extern_cxx_macro, pfx + "_EXTERN_CXX_DECL", pfx + "_EXTERN_DECL");
    }
    out += std::format("#include \"{}\"\n",
                       body_include.empty()
                           ? std::format("../{}.cc", fs_stem)
                           : body_include);
    result.module_content = std::move(out);

    std::string dual_body;
    // The same body is compiled two ways. Included by the module unit above it
    // is attached to the module and needs nothing; compiled on its own with
    // modules turned on it is an ordinary translation unit, and an ordinary
    // translation unit sees the declarations it defines only by importing
    // them. That second form is what an implementation needs when it cannot
    // attach implementations to a module at all — the entities are in the
    // global module (see the extern "C++" wrapping), so defining them here is
    // exactly right.
    //
    // Its own module comes first: `module X;` implied that import, and without
    // the module declaration nothing else does.
    dual_body += std::format("#if defined({}) && !defined({})\n",
                             use_modules_macro, module_unit_macro);
    dual_body += std::format("import {};\n", module_name.str());
    if (import_std)
      dual_body += std::format("import {};\n",
                               std_module_name(module_replaces));
    for (auto &m : unique_imports)
      dual_body += std::format("import {};\n", m);
    dual_body += "#endif\n";

    open_extern(dual_body);
    dual_body += body;
    close_extern(dual_body);
    result.content = std::move(dual_body);
    return result;
  }

  open_extern(out);
  out += body;
  close_extern(out);

  result.content = std::move(out);
  return result;
}
