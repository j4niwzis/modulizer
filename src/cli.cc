module;

export module modulizer.cli;
import modulizer.analyzer;
import modulizer.consumer_rewriter;
import modulizer.consumer_trace;
import modulizer.cross_module;
import modulizer.header_rewriter;
import modulizer.include_analysis;
import modulizer.macro_analyzer;
import modulizer.naming;
import modulizer.rewrite_orchestration;
import modulizer.util;
import modulizer.wrapper_gen;
import libtooling;
import std;

namespace modulizer {

// The module root (everything before the first dot) is the library name, e.g.
// `mylib.core` → `mylib`.
std::string library_name_of(llvm::StringRef module_name) {
  auto dot = module_name.find('.');
  return dot == llvm::StringRef::npos ? module_name.str()
                                      : module_name.substr(0, dot).str();
}

// Parse the CLI into a CommonOptionsParser, printing the error and returning
// nullopt on failure. Shared by every run_* entry point.
 std::optional<clang::tooling::CommonOptionsParser> open_options_parser(
     int argc, const char **argv) {
   auto ep = clang::tooling::CommonOptionsParser::create(
       argc, argv, llvm::cl::getGeneralCategory());
   if (!ep) {
     llvm::errs() << ep.takeError();
     return std::nullopt;
   }
   return std::move(*ep);
 }

bool write_file(llvm::StringRef path, llvm::StringRef content) {
  std::error_code ec;
  llvm::raw_fd_ostream f(std::string(path), ec);
  if (ec) {
    llvm::errs() << "error: " << ec.message() << " (" << path << ")\n";
    return false;
  }
  f << content;
  return true;
}

// Ensure the parent directory of `path` exists.
bool ensure_parent_dir(llvm::StringRef path) {
  auto parent = std::filesystem::path(path.str()).parent_path();
  if (parent.empty()) return true;
  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  if (ec) {
    llvm::errs() << "error: " << ec.message() << " (" << parent << ")\n";
    return false;
  }
  return true;
}

export class ListFunctionsConsumer : public clang::ASTConsumer {
public:
  bool HandleTopLevelDecl(clang::DeclGroupRef dg) override {
    for (auto *d : dg) {
      if (auto *fd = llvm::dyn_cast<clang::FunctionDecl>(d)) {
        if (fd->hasBody()) {
          llvm::outs() << "Function: " << fd->getQualifiedNameAsString() << "\n";
        }
      }
    }
    return true;
  }
};

export class ListFunctionsAction : public clang::ASTFrontendAction {
public:
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
      clang::CompilerInstance &ci, llvm::StringRef) override {
    return std::make_unique<ListFunctionsConsumer>();
  }
};

export inline int run(int argc, const char **argv) {
  auto parser_opt = open_options_parser(argc, argv);
  if (!parser_opt) return 1;
  auto &parser = *parser_opt;

  clang::tooling::ClangTool tool(parser.getCompilations(),
                                 parser.getSourcePathList());

  return tool.run(
      clang::tooling::newFrontendActionFactory<ListFunctionsAction>().get());
}

llvm::cl::opt<std::string> ModuleNameOpt(
    "module-name",
    llvm::cl::desc("Output module name"));

llvm::cl::opt<std::string> OutputDirOpt(
    "output-dir",
    llvm::cl::desc("Output directory"));

llvm::cl::opt<bool> ListModeOpt(
    "list",
    llvm::cl::desc("List extracted entities (dry-run)"));

llvm::cl::opt<bool> CombinedMacrosOpt(
    "combined-macros",
    llvm::cl::desc("Merge all extracted macros into one file"));

llvm::cl::opt<bool> NoExternCxxOpt(
    "no-extern-cxx",
    llvm::cl::desc("Do not wrap module body in extern \"C++\" (headers rewrite)"));

llvm::cl::opt<bool> ExternCxxOpt(
    "extern-cxx",
    llvm::cl::desc("Wrap module bodies in extern \"C++\" (full rewrite mode; "
                   "off by default)"));

llvm::cl::opt<bool> ExportAllOpt(
    "export-all",
    llvm::cl::desc("Export internal/detail entities (for inter-module API use)"));

llvm::cl::opt<bool> ImportStdOpt(
    "import-std",
    llvm::cl::desc("Add import std; to every module (resolves system headers)"));

llvm::cl::opt<bool> CCOnlyOpt(
    "cc-only",
    llvm::cl::desc("Rewrite headers entirely into module interface units "
                   "(.cc); emit no .h and drop includes that can be replaced "
                   "with imports"));

llvm::cl::opt<std::string> ReachableFqnsOpt(
    "reachable-fqn",
    llvm::cl::desc("Comma-separated FQNs to export despite internal namespace"));

llvm::cl::opt<std::string> InternalModeOpt(
    "internal-mode",
    llvm::cl::desc("How to detect internal modules: dir, stem, or both "
                   "(default: both)"));

llvm::cl::list<std::string> ModuleReplacesOpt(
    "module-replaces",
    llvm::cl::desc("Mark a module as replacing headers, format "
                   "module=header1,header2 (repeatable); includes replaced by "
                   "an imported module are removed from the GMF"));

llvm::cl::list<std::string> ConsumerSourcesOpt(
    "consumer-source",
    llvm::cl::desc("Extra source file to trace as a PUBLIC consumer of the "
                   "library. Internal entities it references are exported from "
                   "their defining modules AND re-exported by the generated "
                   "wrapper module. Repeatable; consumers are not rewritten."));

llvm::cl::list<std::string> InternalConsumerSourcesOpt(
    "internal-consumer-source",
    llvm::cl::desc("Extra source file to trace as an INTERNAL consumer of the "
                   "library (e.g. the library's own tests/main, or a sibling "
                   "library consuming the current one). Internal entities it "
                   "references are exported from their defining modules (so the "
                   "consumer works via module imports) but are NOT re-exported "
                   "by the generated wrapper module. Repeatable."));

llvm::cl::list<std::string> LibraryHeadersOpt(
    "library-header",
    llvm::cl::desc("(--consumers mode) Library header used to build the "
                   "include-path to module-name mapping. When given, ALL "
                   "positional files are treated as consumers to rewrite "
                   "(including .h consumer/helper files); otherwise library "
                   "headers are identified by extension. Repeatable."));

llvm::cl::opt<std::string> MacroPrefixOpt(
    "macro-prefix",
    llvm::cl::desc("Override the LIB prefix used in the LIB_USE_MODULES / "
                   "LIB_IMPORT_STD / LIB_USE_IMPORT_STD / LIB_EXPORT macros; "
                   "defaults to the uppercased library name"));

llvm::cl::opt<bool> HyphenMacrosOpt(
    "hyphen-macros",
    llvm::cl::desc("Name generated macro headers <stem>-macros.h instead of "
                    "<stem>_macros.h (Google C++ style)"));

llvm::cl::opt<bool> WrapperModuleOpt(
    "wrapper-module",
    llvm::cl::desc("Rename the main umbrella module to <lib>.umbrella and "
                   "generate a <lib> wrapper module that imports it and "
                   "re-exports only the public API plus consumer-/macro-"
                   "reachable internals"));

// Write a header rewrite's shared export header (once per library), emitting
// the ` + path` trailer. Returns false on write failure.
bool write_export_header(const HeaderRewriteResult &r,
                         std::set<std::string> &written_export_headers) {
  if (r.export_h_content.empty() ||
      written_export_headers.count(r.export_h_name))
    return true;
  written_export_headers.insert(r.export_h_name);
  std::string e_path = std::format("{}/{}.h",
      OutputDirOpt.getValue(), r.export_h_name);
  if (!ensure_parent_dir(e_path)) return false;
  if (!write_file(e_path, r.export_h_content)) return false;
  llvm::outs() << " + " << e_path;
  return true;
}

// Write a header rewrite's generated macros file, emitting the ` + path`
// trailer. Returns false on write failure (a header with no macros is not an
// error).
bool write_macro_file(const HeaderRewriteResult &r) {
  if (r.macros_content.empty()) return true;
  std::string m_path = std::format("{}/{}.h",
      OutputDirOpt.getValue(), r.macros_name);
  if (!ensure_parent_dir(m_path)) return false;
  if (!write_file(m_path, r.macros_content)) return false;
  llvm::outs() << " + " << m_path;
  return true;
}

// Parse --module-replaces "module=header1,header2" into a map.
std::map<std::string, std::set<std::string>> parse_module_replaces() {
  std::map<std::string, std::set<std::string>> module_replaces;
  for (auto &spec : ModuleReplacesOpt) {
    auto eq = spec.find('=');
    if (eq == std::string::npos) continue;
    auto mod = spec.substr(0, eq);
    std::set<std::string> headers;
    std::string cur;
    for (char c : std::string_view(spec).substr(eq + 1)) {
      if (c == ',') {
        if (!cur.empty()) headers.insert(std::move(cur));
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) headers.insert(std::move(cur));
    module_replaces[mod] = std::move(headers);
  }
  return module_replaces;
}

// Parse manually specified FQNs from the --reachable-fqn option.
std::vector<std::string> parse_reachable_fqns() {
  std::vector<std::string> out;
  auto val = ReachableFqnsOpt.getValue();
  if (val.empty()) return out;
  std::size_t pos = 0;
  while (pos < val.size()) {
    auto comma = val.find(',', pos);
    out.push_back(
        std::string(val.substr(pos, comma == std::string::npos
                                       ? std::string::npos : comma - pos)));
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

export inline int run_wrapper(int argc, const char **argv) {
  auto parser_opt = open_options_parser(argc, argv);
  if (!parser_opt) return 1;
  auto &parser = *parser_opt;

  std::vector<std::string> header_paths;
  for (auto &s : parser.getSourcePathList())
    header_paths.push_back(std::string(s));

  auto model = analyze_headers_with_cdb(parser.getCompilations(), header_paths,
                                        /*wrapper_mode=*/true);

  if (ListModeOpt) {
    llvm::outs() << "Entities found:\n";
    for (const auto &item : model.items) {
      std::string kind;
      switch (item.kind) {
        case EntityItem::kClass:    kind = "class"; break;
        case EntityItem::kStruct:   kind = "struct"; break;
        case EntityItem::kFunction: kind = "function"; break;
        case EntityItem::kEnum:     kind = "enum"; break;
        case EntityItem::kAlias:    kind = "alias"; break;
        case EntityItem::kVariable: kind = "variable"; break;
        case EntityItem::kUsingDecl: kind = "using-decl"; break;
        case EntityItem::kUsingDirective: kind = "using-directive"; break;
        default: std::unreachable();
      }
      std::string path;
      for (auto &s : item.ns_path) { path += s; path += "::"; }
      llvm::outs() << "  [" << kind << "] " << path << item.name << "\n";
    }
    llvm::outs() << "Macros: " << model.macros.size() << "\n";
    return 0;
  }

  if (ModuleNameOpt.empty()) {
    llvm::errs() << "error: --module-name is required\n";
    return 1;
  }

  auto reachable = analyze_macro_reachability(model.macros, model,
      kDefaultInternalFilter);
  expand_overloads(reachable, model);
  expand_transitive_types(reachable, model);

  auto cc = generate_wrapper_cc(ModuleNameOpt, header_paths, model,
                                kDefaultInternalFilter,
                                reachable.reachable_fqns);
  auto h = generate_companion_h(model);

  std::string out_dir = OutputDirOpt.empty() ? "." : OutputDirOpt.getValue();
  std::string cc_path = out_dir + "/" + ModuleNameOpt.getValue() + ".cc";
  std::string h_path = out_dir + "/" + ModuleNameOpt.getValue() + "_macros.h";

  std::error_code ec;
  llvm::raw_fd_ostream cc_file(cc_path, ec);
  if (ec) {
    llvm::errs() << "error: " << ec.message() << " (" << cc_path << ")\n";
    return 1;
  }
  cc_file << cc;

  if (!h.empty()) {
    llvm::raw_fd_ostream h_file(h_path, ec);
    if (ec) {
      llvm::errs() << "error: " << ec.message() << " (" << h_path << ")\n";
      return 1;
    }
    h_file << h;
  }

  llvm::outs() << "Generated " << cc_path;
  if (!h.empty()) llvm::outs() << " + " << h_path;
  llvm::outs() << "\n";

  return 0;
}

export inline int run_headers_rewrite(int argc, const char **argv) {
  auto parser_opt = open_options_parser(argc, argv);
  if (!parser_opt) return 1;

  if (ModuleNameOpt.empty()) {
    llvm::errs() << "error: --module-name is required\n";
    return 1;
  }
  if (OutputDirOpt.empty()) {
    llvm::errs() << "error: --output-dir is required\n";
    return 1;
  }

  auto &parser = *parser_opt;

  // Collect source paths and build a name→path map for consumer tracing
  std::vector<std::string> header_paths;
  for (auto &s : parser.getSourcePathList())
    header_paths.push_back(std::string(s));

  auto library_name = library_name_of(ModuleNameOpt.getValue());

  // Auto-trace consumer reachability. The library's own headers (self-consumers)
  // and any explicit consumer sources contribute internal entities that must be
  // exported from their defining modules so the consumers work via module
  // imports. Headers-rewrite mode has no wrapper, so public and internal
  // consumers are merged into one reachable set here (the public/internal split
  // only affects --full, where the wrapper re-exports public-reachable entities
  // only).
  std::map<std::string, std::vector<std::string>> auto_reachable;
  std::vector<std::string> all_extra;
  if (!header_paths.empty()) {
    all_extra = first_extra_args(parser.getCompilations(), header_paths);
    if (ReachableFqnsOpt.getValue().empty() && header_paths.size() > 1) {
      auto_reachable = trace_consumer_reachability(
          header_paths, library_name, all_extra);
    }
    // Explicit consumer sources (the library's own tests/main): internal
    // entities they reference must be exported so they work via module imports.
    // Traced even when --reachable-fqn is set (the manual list is merged, not a
    // replacement).
    std::vector<std::string> public_sources(ConsumerSourcesOpt.begin(),
                                            ConsumerSourcesOpt.end());
    std::vector<std::string> internal_sources(
        InternalConsumerSourcesOpt.begin(), InternalConsumerSourcesOpt.end());
    trace_and_merge(auto_reachable, header_paths, library_name, all_extra,
                    public_sources);
    trace_and_merge(auto_reachable, header_paths, library_name, all_extra,
                    internal_sources);
  }

  auto module_replaces = parse_module_replaces();

  // Macro reachability: internal entities referenced by public macros
  // must be exported so module consumers can expand those macros. This
  // runs ALWAYS (even when `--reachable-fqn` is set — the manual list is
  // merged in, never a replacement), because macro-reachable internals
  // would otherwise be silently lost for consumers.
  std::vector<std::string> macro_reachable;
  std::set<std::string> macro_modules;
  if (!header_paths.empty()) {
    auto mm = compute_macro_modules(
        header_paths, library_name, all_extra, parse_reachable_fqns());
    macro_reachable = std::move(mm.macro_reachable);
    macro_modules = std::move(mm.macro_modules);
    llvm::outs() << "Macro-reachable entities: " << macro_reachable.size()
                  << "\n";
  }

  // Merge ALL auto-traced FQNs (consumer-reachable internal entities), not just
  // this source's own: an entity exported by its defining module must be
  // recognised as exported in every module that references it, so no
  // `extern "C++"` shared-entity declaration is injected next to it.
  std::vector<std::string> reachable_fqns = parse_reachable_fqns();
  for (auto &[producer, fqns] : auto_reachable)
    reachable_fqns.append_range(fqns);
  // Add macro-reachable FQNs (internal entities referenced by public macros)
  reachable_fqns.append_range(macro_reachable);

  RewriteBatchConfig cfg;
  cfg.module_name = ModuleNameOpt.getValue();
  cfg.library_name = library_name;
  cfg.macro_modules = macro_modules;
  cfg.module_replaces = module_replaces;
  cfg.combined_macros = CombinedMacrosOpt;
  cfg.extern_cxx = !NoExternCxxOpt;
  cfg.import_std = ImportStdOpt.getValue();
  cfg.cc_only = CCOnlyOpt.getValue();
  cfg.macro_prefix_override = MacroPrefixOpt.getValue();
  cfg.hyphen_macros = HyphenMacrosOpt.getValue();
  cfg.internal_mode = parse_internal_mode(InternalModeOpt.getValue());

  // The parallel per-header rewrite and the cross-file macro-marker routing
  // second pass happen inside rewrite_header_batch; file writing and the
  // combined-macros / export-header bookkeeping stay sequential here.
  auto h_outcomes = rewrite_header_batch(
      header_paths, reachable_fqns, cfg, parser.getCompilations());

  bool combined = CombinedMacrosOpt;
  std::string combined_macros;
  std::string combined_name;
  std::set<std::string> written_export_headers;

  for (auto &o : h_outcomes) {
    if (!o.ok) return 1;
    auto &fs_stem = o.fs_stem;
    auto &r = o.r;
    std::string h_path = std::format("{}/{}.h", OutputDirOpt.getValue(), fs_stem);
    std::string cc_path = std::format("{}/{}.cc", OutputDirOpt.getValue(), fs_stem);

    if (CCOnlyOpt.getValue()) {
      if (!write_file(cc_path, r.cc_content)) return 1;
      llvm::outs() << "Generated " << cc_path;
    } else {
      if (!write_file(h_path, r.h_content)) return 1;
      if (!write_file(cc_path, r.cc_content)) return 1;
      llvm::outs() << "Generated " << h_path << " + " << cc_path;
    }

    if (!write_export_header(r, written_export_headers)) return 1;

    if (combined) {
      if (!r.macros_content.empty()) {
        combined_macros += r.macros_content;
        combined_name = r.macros_name;
      }
    } else if (!write_macro_file(r)) {
      return 1;
    }

    llvm::outs() << "\n";
  }

  if (combined && !combined_macros.empty()) {
    std::string m_path = std::format("{}/{}.h",
        OutputDirOpt.getValue(), combined_name);
    if (!ensure_parent_dir(m_path)) return 1;
    if (!write_file(m_path, combined_macros)) return 1;
    // Replace per-header #include "stem_macros.h" with combined name
    // This is a simplification; the .h already has the per-stem include,
    // which gets combined into one. Ideally we'd fix the #include.
    llvm::outs() << "Combined macros: " << m_path << "\n";
  }

  return 0;
}

// Consumers-rewrite mode: convert consumer source files (e.g. a library's own
// tests, or a user's app) from `#include "lib/header.h"` to `import lib.module;`
// plus the generated macro-header includes. Standard-library headers are either
// replaced by `import std;` (--import-std) or emitted as an explicit include
// block. Files are rewritten in place. The library headers (for the
// include→module mapping) are identified by extension, or listed explicitly
// with --library-header (in which case ALL positional files — including .h
// consumer/helper headers — are rewritten).
export inline int run_consumers_rewrite(int argc, const char **argv) {
  auto parser_opt = open_options_parser(argc, argv);
  if (!parser_opt) return 1;
  if (ModuleNameOpt.empty()) {
    llvm::errs() << "error: --module-name is required\n";
    return 1;
  }
  auto &parser = *parser_opt;

  std::vector<std::string> header_paths, consumer_paths;
  if (!LibraryHeadersOpt.empty()) {
    for (auto &h : LibraryHeadersOpt) header_paths.push_back(h);
    for (auto &s : parser.getSourcePathList())
      consumer_paths.push_back(std::string(s));
  } else {
    for (auto &s : parser.getSourcePathList()) {
      auto ext = std::filesystem::path(std::string(s)).extension().string();
      if (ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx")
        header_paths.push_back(std::string(s));
      else
        consumer_paths.push_back(std::string(s));
    }
  }
  if (consumer_paths.empty()) {
    llvm::errs() << "error: --consumers requires consumer source files\n";
    return 1;
  }

  auto library_name = library_name_of(ModuleNameOpt.getValue());

  auto all_extra = first_extra_args(parser.getCompilations(), consumer_paths);

  // Which library headers produce a generated macro file? A consumer must
  // include those explicitly (macros do not cross module boundaries).
  std::set<std::string> headers_with_macros;
  {
    std::vector<EntityModel> models(header_paths.size());
    parallel_for(header_paths.size(), [&](std::size_t i) {
      models[i] = analyze_files_with_flags({header_paths[i]}, all_extra);
    });
    for (std::size_t i = 0; i < header_paths.size(); ++i)
      if (!models[i].macros.empty()) headers_with_macros.insert(header_paths[i]);
  }

  auto include_map = build_include_module_map(
      header_paths, library_name, all_extra, HyphenMacrosOpt.getValue(),
      headers_with_macros);
  ConsumerRewriteOptions cfg;
  cfg.import_std = ImportStdOpt.getValue();
  cfg.include_to_module = std::move(include_map);

  // The consumer references internal entities (e.g. entities the library's
  // macros expand to) that are exported from their defining sub-modules but
  // NOT re-exported by the wrapper (internal-consumer reachability is not
  // public API). Trace the consumers against the library headers and import
  // the modules that own the entities each consumer reaches — no manual
  // import needed. Skipped once every consumer is already converted (library
  // includes gone), which keeps the pass idempotent across the conversion
  // passes.
  std::map<std::string, std::set<std::string>> traced_modules_by_consumer;
  // Per-consumer set of system C/POSIX headers whose macros/symbols the
  // consumer uses directly. `import std.compat;` cannot provide the C library's
  // macros or POSIX declarations/types, so those includes are re-added by the
  // rewriter once the library headers no longer pull them in textually.
  std::map<std::string, std::set<std::string>> system_includes_by_consumer;
  {
    bool any_library_include = false;
    for (auto &c : consumer_paths) {
      for (auto &inc : parse_includes(read_file(c)))
        if (cfg.include_to_module.count(inc.path)) {
          any_library_include = true;
          break;
        }
      if (any_library_include) break;
    }
    // Consumers an earlier pass already converted (a sibling library's
    // --consumers run, or a re-run of this one) no longer parse: their
    // `import lib.x;` needs a BMI that does not exist while the library is
    // still being converted, so the parse would die and this pass would
    // silently trace nothing for them. Parse a reconstruction of the
    // pre-module source instead, keyed by the file's own path so relative
    // includes still resolve.
    std::map<std::string, std::string> virtual_sources;
    for (auto &c : consumer_paths) {
      auto src = read_file(c);
      auto restored = demodularize_consumer_source(src, cfg.include_to_module);
      if (restored != src) virtual_sources[c] = std::move(restored);
    }

    if (any_library_include && !header_paths.empty()) {
      auto traced =
          trace_consumer_modules(header_paths, consumer_paths, library_name,
                                 all_extra, &virtual_sources);
      for (auto &[c, producers] : traced) {
        std::set<std::string> mods;
        for (auto &hp : producers)
          mods.insert(derive_module_name(hp, library_name));
        traced_modules_by_consumer[c] = std::move(mods);
      }
      system_includes_by_consumer = trace_consumer_system_includes(
          consumer_paths, all_extra, &virtual_sources);
    }
  }

  for (auto &c : consumer_paths) {
    auto src = read_file(c);
    if (src.empty()) {
      llvm::errs() << "error: cannot read " << c << "\n";
      return 1;
    }
    auto per_cfg = cfg;
    auto it = traced_modules_by_consumer.find(c);
    if (it != traced_modules_by_consumer.end())
      per_cfg.traced_imports.assign(it->second.begin(), it->second.end());
    auto sit = system_includes_by_consumer.find(c);
    if (sit != system_includes_by_consumer.end())
      per_cfg.required_system_includes = sit->second;
    auto out = rewrite_consumer_source(src, per_cfg);
    if (out != src) {
      if (!write_file(c, out)) return 1;
      llvm::outs() << "Converted " << c << "\n";
    } else {
      llvm::outs() << "Unchanged " << c << "\n";
    }
  }

  return 0;
}

// Full rewrite mode: headers become module interface units and implementation
// files (.cpp/.cc/.cxx) become module implementation units. Extern "C++"
// wrapping is OFF by default (enable with --extern-cxx).
export inline int run_full_rewrite(int argc, const char **argv) {
  auto parser_opt = open_options_parser(argc, argv);
  if (!parser_opt) return 1;
  if (ModuleNameOpt.empty()) {
    llvm::errs() << "error: --module-name is required\n";
    return 1;
  }
  if (OutputDirOpt.empty()) {
    llvm::errs() << "error: --output-dir is required\n";
    return 1;
  }
  auto &parser = *parser_opt;

  std::vector<std::string> header_paths, source_paths;
  for (auto &s : parser.getSourcePathList()) {
    auto ext = std::filesystem::path(std::string(s)).extension().string();
    if (ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx")
      header_paths.push_back(std::string(s));
    else
      source_paths.push_back(std::string(s));
  }

  auto library_name = library_name_of(ModuleNameOpt.getValue());

  // Consumer reachability is split into two groups:
  //  - INTERNAL consumers (the library's own headers/impl sources, its own
  //    tests/main, and sibling libraries consuming the current one). Internal
  //    entities they reference must be exported from their defining modules so
  //    those consumers work via module imports, but they are NOT public API and
  //    must NOT be re-exported by the generated wrapper.
  //  - PUBLIC consumers (--consumer-source, i.e. external code). Internal
  //    entities they reference are also exported from their defining modules
  //    AND are re-exported by the wrapper.
  std::map<std::string, std::vector<std::string>> internal_reachable;
  std::map<std::string, std::vector<std::string>> public_reachable;
  // Consumers include implementation files, so trace over the full set.
  std::vector<std::string> trace_paths = header_paths;
  trace_paths.insert(trace_paths.end(), source_paths.begin(), source_paths.end());
  auto all_extra = first_extra_args(parser.getCompilations(), trace_paths);

  if (ReachableFqnsOpt.getValue().empty() && trace_paths.size() > 1) {
    internal_reachable = trace_consumer_reachability(
        trace_paths, library_name, all_extra);
  }
  // Explicit INTERNAL consumer sources (the library's own tests/main, or a
  // sibling library): entities they reference are exported from their defining
  // modules but not re-exported by the wrapper.
  std::vector<std::string> internal_sources(
      InternalConsumerSourcesOpt.begin(), InternalConsumerSourcesOpt.end());
  trace_and_merge(internal_reachable, header_paths, library_name, all_extra,
                  internal_sources);
  // Explicit PUBLIC consumer sources: entities they reference are exported AND
  // re-exported by the wrapper.
  std::vector<std::string> public_sources(ConsumerSourcesOpt.begin(),
                                          ConsumerSourcesOpt.end());
  trace_and_merge(public_reachable, header_paths, library_name, all_extra,
                  public_sources);

  auto module_replaces = parse_module_replaces();

  std::vector<std::string> macro_reachable;
  std::set<std::string> macro_modules;
  if (!header_paths.empty()) {
    auto mm = compute_macro_modules(
        header_paths, library_name, all_extra, parse_reachable_fqns());
    macro_reachable = std::move(mm.macro_reachable);
    macro_modules = std::move(mm.macro_modules);
    llvm::outs() << "Macro-reachable entities: " << macro_reachable.size()
                  << "\n";
  }

  // FQNs of classes with complete definitions anywhere in the library. A
  // forward declaration whose class is defined by another header must not be
  // exported from its own module (that module exports the definition); opaque
  // types that are never defined stay exported.
  std::vector<std::string> defined_fqns;
  std::vector<std::string> fwd_declared_fqns;
  std::vector<std::string> alias_reachable;
  std::set<std::string> same_module_free_fqns;
  if (!header_paths.empty()) {
    auto all = analyze_files_with_flags(header_paths, all_extra);
    std::set<std::string> seen;
    for (auto &item : all.items) {
      if (!item.complete) continue;
      auto fqn = fqn_of(item.ns_path, item.name);
      if (seen.insert(fqn).second) defined_fqns.push_back(fqn);
    }
    // Entities forward-declared in one header but defined in a different one
    // are declared in modules that do not define them. Their declarations and
    // definitions must be `extern "C++"` to stay a single shared entity.
    fwd_declared_fqns = cross_module_fwd_declared_fqns(header_paths, all_extra);
    // Entities referenced inside template bodies in one header but defined in a
    // different one (e.g. a declaration macro expanding to a call that reaches
    // an internal helper). A consumer instantiating those templates resolves the
    // name at the point of definition in the USING module, so that module
    // needs a module-local `extern "C++"` declaration and the defining module
    // must mark the definition `extern "C++"` — the entity itself is internal
    // and never exported to consumers.
    {
      auto tpl_refs = cross_module_template_body_referenced_fqns(
          header_paths, all_extra);
      // Skip entities that are already exported (macro-reachable or listed via
      // `--reachable-fqn`): consumers see those through the module import, so
      // the template body resolves them without an injected `extern "C++"`
      // declaration. Only genuinely internal helpers (like a matcher helper)
      // need the shared-entity treatment.
      std::set<std::string> exported;
      for (auto &r : macro_reachable) exported.insert(r);
      for (auto &r : parse_reachable_fqns()) exported.insert(r);
      // Consumer-traced entities are exported too (the library's own tests/main
      // use them via module imports), so they must not get the injected
      // `extern "C++"` shared-entity treatment.
      for (auto &[producer, fqns] : internal_reachable)
        for (auto &f : fqns) exported.insert(f);
      for (auto &[producer, fqns] : public_reachable)
        for (auto &f : fqns) exported.insert(f);
      auto not_exported = [&](const std::string &f) {
        // With --export-all every library entity is exported, so no injected
        // `extern "C++"` declaration is needed: using modules see the entity
        // through the defining module's export.
        if (ExportAllOpt.getValue()) return false;
        if (exported.count(f)) return false;
        for (auto &e : exported)
          if (e.ends_with("::" + f) || f.ends_with("::" + e)) return false;
        return true;
      };
      // Alias templates cannot be forward-declared: exporting them from the
      // defining module makes them visible to using modules via the import —
      // no injected copy needed.
      for (auto &f : tpl_refs.aliases)
        if (not_exported(f)) alias_reachable.push_back(f);
      // Forward-declarable entities (function/class templates, functions,
      // classes) are re-declared module-locally via `extern "C++"`.
      for (auto &f : tpl_refs.fwd_declared)
        if (not_exported(f)) fwd_declared_fqns.push_back(f);
    }
    // Classes whose members are defined out-of-line in implementation files:
    // those impl units live in different modules, so the class must be
    // `extern "C++"` for the member definition to be valid across modules.
    if (!source_paths.empty()) {
      // A source whose stem matches a library header is that header's own
      // implementation unit: it lives in the SAME module as the entities it
      // defines, so its free functions/variables and class-member definitions
      // need no `extern "C++"`. Only loose implementation files (no same-stem
      // header) belong to a different module and must be flagged.
      std::set<std::string> header_stems;
      for (auto &h : header_paths)
        header_stems.insert(std::filesystem::path(h).stem().string());
      auto member_classes = out_of_line_member_class_fqns(
          source_paths, all_extra, header_stems);
      fwd_declared_fqns.insert(fwd_declared_fqns.end(),
                               member_classes.begin(), member_classes.end());
      // Free functions/variables defined in impl files but declared in a
      // library header: both sides must be `extern "C++"` so they stay a
      // single shared entity. Same-module impl sources (stem matches a header)
      // are handled inside out_of_line_defined_free_fqns.
      auto free_fqns = out_of_line_defined_free_fqns(source_paths, all_extra);
      fwd_declared_fqns.insert(fwd_declared_fqns.end(),
                               free_fqns.begin(), free_fqns.end());
      // Classes defined entirely in a source file and friend-declared in a
      // library header (e.g. a result class friends an internal executor class,
      // defined in an impl unit) must be `extern "C++"` on both sides.
      auto free_classes = out_of_line_defined_free_classes(
          source_paths, all_extra);
      fwd_declared_fqns.insert(fwd_declared_fqns.end(),
                               free_classes.begin(), free_classes.end());
      // Free functions/variables defined in a SAME-module impl source (stem
      // matches a header): the interface declaration is a normal module
      // entity that the impl unit defines. Record them so header rewriting
      // keeps such declarations exported (not `extern "C++"`).
      std::vector<std::string> same_stem_sources;
      for (auto &s : source_paths)
        if (header_stems.count(std::filesystem::path(s).stem().string()))
          same_stem_sources.push_back(s);
      for (auto &f : out_of_line_defined_free_fqns(same_stem_sources, all_extra,
                                                   /*collect_same_module=*/true))
        same_module_free_fqns.insert(f);
    }
    // Entities friend-declared inside an extern "C++" class attach to the
    // global module, so their definitions must be `extern "C++"` too, wherever
    // they live. The impl side needs these FQNs to mark the definitions.
    auto friend_fqns = friend_extern_fqns(
        header_paths, all_extra, fwd_declared_fqns);
    fwd_declared_fqns.insert(fwd_declared_fqns.end(),
                             friend_fqns.begin(), friend_fqns.end());
  }

  bool extern_cxx = ExternCxxOpt;

  // Merge ALL consumer-traced FQNs from BOTH groups (see run_headers_rewrite
  // for rationale): internal and public consumers both need their referenced
  // internal entities exported from the defining modules.
  std::vector<std::string> reachable_fqns = parse_reachable_fqns();
  for (auto &[producer, fqns] : internal_reachable)
    reachable_fqns.append_range(fqns);
  for (auto &[producer, fqns] : public_reachable)
    reachable_fqns.append_range(fqns);
  reachable_fqns.append_range(macro_reachable);
  reachable_fqns.append_range(alias_reachable);

  RewriteBatchConfig cfg;
  cfg.module_name = ModuleNameOpt.getValue();
  cfg.library_name = library_name;
  cfg.macro_modules = macro_modules;
  cfg.module_replaces = module_replaces;
  cfg.defined_fqns = defined_fqns;
  cfg.fwd_declared_fqns = fwd_declared_fqns;
  cfg.extern_cxx = extern_cxx;
  cfg.import_std = ImportStdOpt.getValue();
  cfg.cc_only = CCOnlyOpt.getValue();
  cfg.same_module_free_fqns = same_module_free_fqns;
  cfg.macro_prefix_override = MacroPrefixOpt.getValue();
  cfg.hyphen_macros = HyphenMacrosOpt.getValue();
  cfg.wrapper_module = WrapperModuleOpt.getValue();
  cfg.internal_mode = parse_internal_mode(InternalModeOpt.getValue());

  // Headers → module interface units. Outputs mirror the header's subdirectory
  // structure under the library root so same-basename headers don't collide.
  // The per-header rewrite is the most expensive step (a full clang parse +
  // rewrite per header), so it runs in parallel inside rewrite_header_batch;
  // file writing and the shared bookkeeping
  // (interface_modules/stem_to_module/macro_files) happen sequentially here.
  auto outcomes = rewrite_header_batch(
      header_paths, reachable_fqns, cfg, parser.getCompilations());

  std::set<std::string> written_export_headers;
  std::set<std::string> interface_modules;
  std::set<std::string> macro_files;
  // Header stem → module name. An implementation file implements the header
  // with the same stem (e.g. src/mylib-filepath.cc implements the class in
  // internal/mylib-filepath.h), so its implementation unit must live in that
  // header's module — a member of a class can only be defined in the module
  // that declares the class.
  std::map<std::string, std::string> stem_to_module;

  for (auto &o : outcomes) {
    if (!o.ok) return 1;
    interface_modules.insert(o.per_file_module);
    stem_to_module[o.fs_stem] = o.per_file_module;
    auto &r = o.r;
    std::string cc_path =
        std::format("{}/{}", OutputDirOpt.getValue(), o.rel_cc);
    if (CCOnlyOpt.getValue()) {
      if (!ensure_parent_dir(cc_path)) return 1;
      if (!write_file(cc_path, r.cc_content)) return 1;
      llvm::outs() << "Generated " << cc_path;
    } else {
      std::string h_path = std::format("{}/{}", OutputDirOpt.getValue(), o.rel);
      if (!ensure_parent_dir(h_path)) return 1;
      if (!write_file(h_path, r.h_content)) return 1;
      if (!write_file(cc_path, r.cc_content)) return 1;
      llvm::outs() << "Generated " << h_path << " + " << cc_path;
    }

    if (!write_export_header(r, written_export_headers)) return 1;
    if (!write_macro_file(r)) return 1;
    if (!r.macros_content.empty())
      macro_files.insert(std::format("{}.h", r.macros_name));
    llvm::outs() << "\n";
  }

  // Implementation files → module implementation units, written under an
  // `impl/` subdirectory (kept separate from the interface units).
  auto impl_dir = std::filesystem::path(OutputDirOpt.getValue()) / "impl";
  std::error_code mkdir_ec;
  std::filesystem::create_directories(impl_dir, mkdir_ec);
  if (mkdir_ec) {
    llvm::errs() << "error: " << mkdir_ec.message() << " (" << impl_dir << ")\n";
    return 1;
  }

  struct ImplOutcome {
    std::string src;
    std::string per_file_module;
    std::string fs_stem;
    SourceRewriteResult r;
    bool ok = false;
  };
  std::vector<ImplOutcome> impl_outcomes(source_paths.size());
  // The per-source rewrite (clang parse + include analysis) runs in parallel;
  // the empty-interface decision and file writes stay sequential because they
  // mutate shared interface_modules.
  parallel_for(source_paths.size(), [&](std::size_t idx) {
    auto &src = source_paths[idx];
    auto &o = impl_outcomes[idx];
    o.src = src;
    auto extra_args = compile_extra_args(parser.getCompilations(), src);
    auto fs_stem = std::filesystem::path(src).stem().string();
    o.fs_stem = fs_stem;
    // An implementation unit lives in the module of the header it implements
    // (matched by stem); fall back to the source path's own derived module.
    auto it = stem_to_module.find(fs_stem);
    auto per_file_module = it != stem_to_module.end()
        ? it->second
        : derive_module_name(src, library_name);
    o.per_file_module = per_file_module;
    auto r = rewrite_source(src, per_file_module, {}, extra_args,
                            ImportStdOpt.getValue(), extern_cxx,
                            module_replaces, macro_files, interface_modules,
                            fwd_declared_fqns, HyphenMacrosOpt.getValue(),
                            WrapperModuleOpt.getValue()
                                ? std::format("{}.umbrella", library_name)
                                : std::string{});
    o.ok = !r.content.empty();
    o.r = std::move(r);
  });

  // Implementation-only modules (no header-provided interface) get an empty
  // interface. The wrapper facade must NOT import these: they are separate
  // targets (e.g. a test-main facade) that depend on the library, so importing
  // them
  // from the wrapper would create a target dependency cycle.
  std::set<std::string> empty_interface_modules;
  for (auto &o : impl_outcomes) {
    if (!o.ok) return 1;
    auto &per_file_module = o.per_file_module;
    auto &fs_stem = o.fs_stem;
    // A module implementation unit needs its module's interface to exist;
    // clang refuses to compile `module X;` without one. Modules that have no
    // header-provided interface (loose .cpp/.cc/.cxx) get an empty interface.
    if (!interface_modules.count(per_file_module)) {
      interface_modules.insert(per_file_module);
      empty_interface_modules.insert(per_file_module);
      std::string iface_path = std::format("{}/{}.cc",
          OutputDirOpt.getValue(), fs_stem);
      if (!write_file(iface_path,
                      std::format("export module {};\n", per_file_module)))
        return 1;
      llvm::outs() << "Generated " << iface_path
                   << " (empty interface for implementation-only module)\n";
    }

    std::string cc_path = std::format("{}/impl/{}.cc",
        OutputDirOpt.getValue(), fs_stem);
    if (!write_file(cc_path, o.r.content)) return 1;
    llvm::outs() << "Generated " << cc_path
                 << " (module implementation unit)\n";
  }

  // Wrapper-module mode: generate a `<lib>` facade module that imports the
  // `<lib>.umbrella` module and re-exports only the public API plus the
  // internal entities reachable from PUBLIC consumers/macros. Internal-consumer
  // reachability (the library's own tests/main/impls) is deliberately excluded:
  // those entities are exported from their defining modules but are not public
  // API, so the wrapper must not expose them.
  if (WrapperModuleOpt.getValue() && !header_paths.empty()) {
    auto model =
        analyze_headers_with_cdb(parser.getCompilations(), header_paths);
    auto wrapper_reachable = wrapper_reachable_fqns(
        parse_reachable_fqns(), macro_reachable, alias_reachable,
        public_reachable);
    auto wrapper_cc = generate_import_wrapper_cc(
        library_name, library_name + ".umbrella", model,
        kDefaultInternalFilter, wrapper_reachable, ImportStdOpt.getValue(),
        [&] {
          std::vector<std::string> mods;
          for (auto &m : interface_modules)
            if (!empty_interface_modules.count(m)) mods.push_back(m);
          return mods;
        }(),
        fwd_declared_fqns);
    std::string w_path =
        std::format("{}/{}.cc", OutputDirOpt.getValue(), library_name);
    if (!write_file(w_path, wrapper_cc)) return 1;
    llvm::outs() << "Generated " << w_path << " (wrapper module)\n";
  }

  return 0;
}

} // namespace modulizer
