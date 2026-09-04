module;

export module modulizer.rewrite_export;
import modulizer.astutil;
import modulizer.rewrite_inject;
import modulizer.rewrite_util;
import modulizer.rewrite_visitors;
import modulizer.util;
import libtooling;
import std;

// The header-rewrite ASTConsumer: runs the export-annotation visitor, the
// used-headers visitor, the friend-of-extern-C++ collector, and the
// forward-declaration injectors, then emits the injected declarations.

export class HeaderRewriteConsumer : public clang::ASTConsumer {
public:
  HeaderRewriteConsumer(clang::ASTContext &ctx,
                        std::vector<ModPoint> &mods, const std::regex &re,
                        std::string_view export_macro,
                        std::string_view shared_export_macro,
                        std::string_view linkage_macro,
                        std::string_view extern_decl_macro,
                        std::string_view inline_macro,
                        const std::vector<std::string> &reachable_fqns,
                        std::vector<std::string> &internal_fqns,
                        bool no_internal_filter = false,
                        std::set<std::string> *used_headers = nullptr,
                        const std::vector<std::string> &defined_fqns = {},
                        std::vector<std::pair<std::vector<std::string>, std::string>> *fwd_decls = nullptr,
                        bool extern_cxx = false,
                        std::string_view extern_cxx_macro = {},
                        const std::vector<std::string> &fwd_declared_fqns = {},
                        const std::set<std::string> &same_module_free_fqns = {},
                        const std::set<std::string> *library_headers = nullptr,
                        std::map<std::string, std::vector<ModPoint>> *external_macro_mods = nullptr,
                        bool gcc_modules = false)
      : ctx(ctx), mods(mods), re(re), export_macro(export_macro),
        shared_export_macro(shared_export_macro),
        linkage_macro(linkage_macro),
        extern_decl_macro(extern_decl_macro), inline_macro(inline_macro),
        reachable_fqns(reachable_fqns), internal_fqns(internal_fqns),
        no_internal_filter(no_internal_filter), used_headers(used_headers),
        defined_fqns(defined_fqns), fwd_decls(fwd_decls),
        extern_cxx(extern_cxx), extern_cxx_macro(extern_cxx_macro),
        fwd_declared_fqns(fwd_declared_fqns),
        same_module_free_fqns(same_module_free_fqns),
        library_headers(library_headers),
        external_macro_mods(external_macro_mods), gcc_modules(gcc_modules) {}

  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    std::set<std::string> friend_extern_fqns;
    if (!fwd_declared_fqns.empty()) {
      std::set<std::string> fwd_set(fwd_declared_fqns.begin(),
                                    fwd_declared_fqns.end());
      FriendExternCxxCollector fv(ctx.getSourceManager(), fwd_set,
                                  friend_extern_fqns);
      fv.TraverseDecl(ctx.getTranslationUnitDecl());
    }
    HeaderVisitor visitor(ctx.getSourceManager(), mods, re, export_macro,
                          shared_export_macro, linkage_macro, extern_decl_macro,
                          inline_macro,
                          reachable_fqns, internal_fqns, no_internal_filter,
                          defined_fqns, extern_cxx, fwd_declared_fqns,
                          friend_extern_fqns, same_module_free_fqns,
                          library_headers, external_macro_mods, gcc_modules);
    visitor.TraverseDecl(ctx.getTranslationUnitDecl());
    if (used_headers) {
      UsedHeadersVisitor uv(ctx.getSourceManager(), *used_headers);
      uv.TraverseDecl(ctx.getTranslationUnitDecl());
    }
    // Collect library entities that this module's code references but that are
    // only forward-declared in an included library header. Each such entity
    // needs its own declaration in this module: exported when the module also
    // defines it (so the later exported definition is consistent), module-local
    // otherwise.
    if (fwd_decls) {
      std::set<std::string> defined_set(defined_fqns.begin(),
                                        defined_fqns.end());
      std::set<std::string> cross_set(fwd_declared_fqns.begin(),
                                      fwd_declared_fqns.end());
      std::map<std::string, clang::NamedDecl *> needed;
      std::vector<std::string> order;
      std::set<std::string> complete_needed;
      FwdDeclRefVisitor fv(ctx.getSourceManager(), defined_set, cross_set,
                           needed, order, complete_needed);
      fv.TraverseDecl(ctx.getTranslationUnitDecl());
      // Emit cross-module template-body helpers in dependency order first
      // (transitive deps come before their dependents), then any remaining
      // forward-declared entities (alphabetical order is fine for those).
      std::map<std::string, clang::NamedDecl *> index;
      DeclIndexVisitor div(ctx.getSourceManager(), index);
      div.TraverseDecl(ctx.getTranslationUnitDecl());
      // The same FQN can reach process() several ways: `fwd_declared_fqns`
      // may list it more than once (an entity can be both an out-of-line-
      // defined free function and a friend-extern fqn — a library's
      // `make_and_register_test_info` is both), and `order` may carry it. process()
      // must run at most once per FQN or the module gets two identical
      // injected declaration blocks.
      std::set<std::string> processed;
      auto process_once = [&](const std::string &fqn, clang::NamedDecl *d) {
        if (!processed.insert(fqn).second) return;
        process_fwd_decl(fqn, d, complete_needed);
      };
      for (auto &fqn : order) {
        auto it = needed.find(fqn);
        clang::NamedDecl *d = it != needed.end() ? it->second : nullptr;
        if (!d) {
          auto ii = index.find(fqn);
          if (ii != index.end()) d = ii->second;
        }
        if (d) process_once(fqn, d);
      }
      for (auto &[fqn, d] : needed) {
        if (!std::ranges::contains(order, fqn) && d)
          process_once(fqn, d);
      }
      // Some `fwd_declared_fqns` entities are referenced only inside OTHER
      // headers' template bodies (e.g. `ApplyImpl` in `Apply`'s trailing
      // return type), so the FwdDeclRefVisitor above never observes them in
      // this file. Inject declarations for those too, using the decl index.
      for (auto &df : fwd_declared_fqns) {
        auto nit = needed.find(df);
        if (nit != needed.end() && nit->second) continue;
        auto it = index.find(df);
        if (it == index.end()) continue;
        process_once(df, it->second);
      }
    }
  }

  // Inject a forward declaration for a cross-module entity referenced by this
  // module's code (see the callers in HandleTranslationUnit). Exported when the
  // module also defines the entity; module-local (extern "C++") otherwise.
  void process_fwd_decl(const std::string &fqn, clang::NamedDecl *d,
                        const std::set<std::string> &complete_needed) {
    // Find the entity's definition. If it lives in this module (main file),
    // the module exports it, so an exported forward declaration is injected
    // before the body. If it lives in another included library header, that
    // header becomes an import exporting the definition, so no forward
    // declaration is needed here. If it is not visible anywhere in this TU, the
    // defining module is not imported: a module-local forward declaration must
    // be injected so this module's own code can reference the entity.
    clang::Decl *def = nullptr;
    if (auto *fd = llvm::dyn_cast<clang::FunctionDecl>(d))
      def = fd->getDefinition();
    else if (auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(d))
      def = ft->getTemplatedDecl()->getDefinition();
    else if (auto *sd =
                 llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(d)) {
      // A specialization is what a reference through a default template
      // argument looks like (`template <class L = mp_list<>>` yields
      // `mp_list<>`), and it is a CXXRecordDecl, so the arm below would claim
      // it. Asking a specialization that nothing instantiated for ITS
      // definition yields none, which reads here as "the defining module is
      // not imported" — and a declaration of the primary template is injected
      // beside the import that already provides it:
      //
      //   error: declaration of 'X' in the global module follows declaration
      //          in module lib.x
      //
      // The primary template is the entity this stands for, so ask that.
      if (auto *ct = sd->getSpecializedTemplate())
        def = ct->getTemplatedDecl()->getDefinition();
    }
    else if (auto *rd = llvm::dyn_cast<clang::CXXRecordDecl>(d))
      def = rd->getDefinition();
    else if (auto *ct = llvm::dyn_cast<clang::ClassTemplateDecl>(d)) {
      def = ct->getTemplatedDecl()->getDefinition();
      if (!def) {
        // A primary class template with only a partial specialization
        // (e.g. `template <typename T> struct Function;` + `template
        // <typename R, typename...> struct Function<R(Args...)>`) is never
        // "complete", so getDefinition() is null even though the partial
        // specialization provides the definition. Treat the partial
        // specialization as the definition for visibility purposes.
        llvm::SmallVector<clang::ClassTemplatePartialSpecializationDecl *, 4>
            partial_specs;
        ct->getPartialSpecializations(partial_specs);
        if (!partial_specs.empty()) def = partial_specs.front();
      }
    }
    bool cross_fwd = false;
    for (auto &df : fwd_declared_fqns)
      if (matches_reachable(fqn, df)) { cross_fwd = true; break; }
    // A COMPLETE internal type needed by an injected signature (e.g.
    // `faketype` in `operator==(faketype, faketype)`) must be injected in full
    // even when its definition is visible via an included header — the defining
    // module does not export it, so this module cannot see it.
    bool keep_full = complete_needed.count(fqn) > 0;
    bool exported = false;
    if (def) {
      if (ctx.getSourceManager().isInMainFile(def->getLocation()))
        exported = true;
      else if (!cross_fwd && !keep_full)
        return;  // definition visible via an imported library header
      else if (visible_via_import(fqn))
        // The definition is visible through an import of its defining module (a
        // public entity is always exported; an internal one is exported when
        // listed as reachable). Injecting an `extern "C++"` declaration here
        // would be a redundant, distinct global-module entity next to the
        // imported definition.
        return;
    }
    if (exported && !cross_fwd && !keep_full) {
      // The definition is in this very module. If it precedes the declaration
      // the reference resolves to, the declaration is redundant: the body
      // defines it. But when the reference goes through a declaration in an
      // imported module that does NOT export the entity (a cross-module
      // `extern "C++"` shared entity, e.g. `ToString` declared in
      // mylib-internal.h, used in a template body, and defined later in
      // mylib-printers.h), that declaration is invisible to this module and an
      // exported forward declaration must be injected so the template body
      // resolves the name.
      if (d && def &&
          ctx.getSourceManager().isBeforeInTranslationUnit(
              def->getLocation(), d->getLocation()))
        return;
    }
    // The declaration this module is about to copy may be the one it already
    // sees: `d` can be the declaration in an INCLUDED library header, which the
    // rewrite turns into an import. Whether it arrives through that import
    // depends on how the tree is BUILT. Wrapped, the declaring module exports
    // it (that is what the shared marker is for) and a copy here is not a spare
    // but a second declaration of the same name, which both compilers reject,
    // each in its own words:
    //
    //   error: declaration of 'X' in the global module follows declaration
    //          in module lib.other
    //   error: ambiguating new declaration of 'X'
    //
    // That holds however the tree is built: the declaration is exported either
    // way, taking `extern "C++"` for itself where the wrapping is absent, so
    // the import supplies it and this copy is never the only way to name it.
    // The wrapping decides how an entity is SPELLED, not who owns it, so this
    // holds either way: the declaring module exports the entity and the import
    // supplies it, so a copy here is redundant. That rests on the declaring
    // module actually exporting it — which it now does in both builds, the
    // forward declaration of a cross-module entity included (see the
    // kKeepPrivate branch in the header visitor). While one build exported it
    // and the other did not, this said "exported" in a tree where it was not,
    // and dropped the copy that was the only way to name it.
    //
    // When a definition IS visible, its file is the one carrying the marker, so
    // ask about that one rather than the declaration this copy was taken from.
    if (library_headers) {
      auto &sm = ctx.getSourceManager();
      auto loc = sm.getExpansionLoc(def ? def->getLocation() : d->getLocation());
      if (loc.isValid() && !sm.isInMainFile(loc)) {
        auto fn = sm.getFilename(loc);
        if (!fn.empty()) {
          auto canon =
              std::filesystem::weakly_canonical(std::filesystem::path(fn.str()))
                  .string();
          // The declaring module exports it either the ordinary way (public, or
          // internal-and-reachable) or as a cross-module declaration, which is
          // exported exactly so importers merge with it instead of copying it.
          bool exported_there =
              visible_via_import(fqn) ||
              std::ranges::any_of(fwd_declared_fqns, [&](const std::string &f) {
                return matches_reachable(fqn, f, /*symmetric=*/true);
              });
          if (library_headers->count(canon) && exported_there) return;
        }
      }
    }
    // The definition lives in another module (not visible in this TU). If that
    // module is imported AND exports the entity (an entity listed as reachable,
    // or with export-all), the reference resolves through the import and the
    // injected declaration would be a redundant, distinct global-module entity
    // — skip it. Note that PUBLIC-namespace visibility does NOT apply here: a
    // bare declaration whose definition is not in this TU is kept private
    // (kKeepPrivate) by its declaring module unless reachable, so only
    // reachable entities are visible via the import. Only entities that are NOT
    // visible via any import need a module-local forward declaration.
    if (!def && is_exported_via_import(fqn)) return;
    // Reproduce the forward declaration inside the namespace it lives in,
    // exported when this module also defines the entity. This mirrors the
    // textual-include world where the declaration was visible anywhere the
    // declaring header was included.
    std::vector<std::string> nss;
    for (auto *dc = d->getDeclContext(); dc; dc = dc->getParent()) {
      if (auto *nd = llvm::dyn_cast<clang::NamespaceDecl>(dc)) {
        if (!nd->isAnonymousNamespace())
          nss.push_back(nd->getNameAsString());
      }
    }
    std::reverse(nss.begin(), nss.end());
    auto &sm = ctx.getSourceManager();
    // When the reference is to the inner record of a class template (e.g. a
    // `friend class NaggyMockImpl;`), reproduce the forward declaration from
    // the described class template so it keeps the `template <...>` header and
    // matches the definition's kind.
    auto *text_decl = d;
    if (auto *rd = llvm::dyn_cast<clang::CXXRecordDecl>(text_decl))
      if (auto *ct = rd->getDescribedClassTemplate()) text_decl = ct;
    auto text = clang::Lexer::getSourceText(
        clang::CharSourceRange::getTokenRange(text_decl->getSourceRange()),
        sm, ctx.getLangOpts());
    if (text.empty()) return;
    std::string decl_text = text.str();
    // A redeclaration must not repeat a default template argument that has
    // already been declared, and the definition this is copied from declares
    // them. Taken verbatim the copy carries them along:
    //
    //   template <typename C, bool = sizeof(Test<C>(0)) == sizeof(Yes)>
    //   struct Impl;
    //
    //   error: redefinition of default argument for 'bool <anonymous>'
    //
    // The declaration arrives from the imported module as well, so the two
    // collide. Rebuild the parameter list from the AST, which drops the
    // defaults and keeps everything that identifies the parameters.
    if (auto *td = llvm::dyn_cast<clang::TemplateDecl>(text_decl)) {
      if (auto *tpl = td->getTemplateParameters()) {
        auto begin = sm.getFileOffset(text_decl->getSourceRange().getBegin());
        auto rangle = sm.getFileOffset(tpl->getRAngleLoc());
        if (rangle > begin && rangle - begin < decl_text.size()) {
          auto policy = ctx.getPrintingPolicy();
          std::string params;
          for (auto *param : *tpl) {
            if (!params.empty()) params += ", ";
            if (auto *tp = llvm::dyn_cast<clang::TemplateTypeParmDecl>(param)) {
              params += tp->wasDeclaredWithTypename() ? "typename" : "class";
              if (tp->isParameterPack()) params += "...";
              if (!tp->getName().empty())
                params += " " + tp->getName().str();
            } else if (auto *nt =
                           llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(
                               param)) {
              params += nt->getType().getAsString(policy);
              if (nt->isParameterPack()) params += "...";
              if (!nt->getName().empty())
                params += " " + nt->getName().str();
            } else {
              // A template template parameter. Printing it is enough here: its
              // own defaults are inside its nested list, which the printer
              // omits with SuppressDefaultTemplateArgs unset only for
              // arguments, not parameters, so fall back to the source form.
              params.clear();
              break;
            }
          }
          if (!params.empty())
            decl_text = std::format("template <{}>{}", params,
                                    decl_text.substr(rangle - begin + 1));
        }
      }
    }
    // For a cross-module entity the referenced declaration is a full DEFINITION
    // in another header (e.g. `bool Helper(const A&, const B&) { return true;
    // }`). This module only needs the declaration so the template body's lookup
    // at its point of definition succeeds; strip the body (everything from the
    // `{` that begins the body) and keep the signature. Use the decl's actual
    // body location so braces inside a trailing return type (`auto f(...) ->
    // decltype(foo{})`) are kept. Function bodies only — a class's `{}` is its
    // definition, kept for complete types and turned into a forward declaration
    // below.
    if (llvm::isa<clang::FunctionDecl>(text_decl) ||
        llvm::isa<clang::FunctionTemplateDecl>(text_decl)) {
      clang::SourceLocation body_loc;
      if (auto *fd = llvm::dyn_cast<clang::FunctionDecl>(text_decl))
        if (auto *body = fd->getBody()) body_loc = body->getBeginLoc();
      if (body_loc.isValid()) {
        auto begin = sm.getFileOffset(text_decl->getBeginLoc());
        auto file_off = sm.getFileOffset(body_loc);
        // `file_off` is absolute; decl_text starts at `begin`.
        if (file_off >= begin && file_off - begin <= decl_text.size()) {
          decl_text.erase(file_off - begin);
        } else {
          auto body_start = decl_text.find('{');
          if (body_start != std::string::npos) decl_text.erase(body_start);
        }
      } else {
        auto body_start = decl_text.find('{');
        if (body_start != std::string::npos) decl_text.erase(body_start);
      }
    }
    // A class/struct injected here must be a forward declaration (`template
    // <typename P> struct negation;`), not the full definition with its
    // base-class list — copying `struct negation : integral_constant<...>;`
    // (the body `{}` was already stripped) leaves a malformed base-clause with
    // no body. Exception: a COMPLETE internal type needed by an injected
    // signature (`faketype` in `operator==(faketype, faketype)`) must keep its
    // full definition — a forward declaration would leave by-value parameters
    // incomplete.
    if (!complete_needed.count(fqn)) {
      bool is_record = llvm::isa<clang::CXXRecordDecl>(text_decl) ||
                       llvm::isa<clang::ClassTemplateDecl>(text_decl);
      if (is_record) {
        // Truncate right after the class name: `template <...> struct Name;` —
        // drop any `final`, base clause, attributes after the name, and any
        // `//` comments that follow. Attributes between the class keyword and
        // the name come in two flavors: literal `[[nodiscard]]` lists AND
        // attribute macros (`class LIB_API_ [[nodiscard]] Foo { ... };`).
        // Both must be dropped. Rather than guess which leading identifier is
        // the macro, locate the class name itself (from the decl) as a whole
        // token after the keyword and erase everything up to it.
        auto kw = decl_text.rfind("struct ");
        std::size_t kw_len = 7;
        if (kw == std::string::npos) {
          kw = decl_text.rfind("class ");
          kw_len = 6;
        }
        if (kw != std::string::npos) {
          auto name_str = text_decl->getName();
          std::size_t at = decl_text.find(name_str, kw);
          while (at != std::string::npos) {
            bool left_ok = at == 0 || !is_ident_char(decl_text[at - 1]);
            bool right_ok = at + name_str.size() >= decl_text.size() ||
                            !is_ident_char(decl_text[at + name_str.size()]);
            if (left_ok && right_ok) break;
            at = decl_text.find(name_str, at + 1);
          }
          if (at != std::string::npos && at > kw + kw_len)
            decl_text.erase(kw + kw_len, at - (kw + kw_len));
          // After the prefix (attributes / attribute macros) is erased, the
          // name sits directly after the keyword; truncate there so base
          // clauses (`: public Base`), `final` and trailing comments drop.
          auto name_end = kw + kw_len + name_str.size();
          if (name_end < decl_text.size()) decl_text.erase(name_end);
        }
      }
    }
    // The source range may end before the terminating ';'.
    auto end = decl_text.find_last_not_of(" \t\r\n");
    if (end != std::string::npos) decl_text.erase(end + 1);
    if (decl_text.empty() || decl_text.back() != ';') decl_text += ";";
    // A function redeclaration must not repeat default arguments (they may be
    // specified only once per parameter per TU); the defining module already
    // provides them, and an injected duplicate is ill-formed.
    if (llvm::isa<clang::FunctionDecl>(text_decl) ||
        llvm::isa<clang::FunctionTemplateDecl>(text_decl))
      strip_function_default_args(decl_text);
    // A declaration written with its own `extern` storage-class specifier
    // (`extern const char X[];`, or a function whose keyword follows the
    // template parameter list) must not keep it: the `extern "C++"` added below
    // would leave the specifier inside the linkage-specification, which is
    // ill-formed. The keyword says nothing a declaration does not already mean,
    // and for a variable the linkage-specification keeps it a declaration
    // rather than a definition.
    clang::Decl *storage_decl = text_decl;
    if (auto *td = llvm::dyn_cast<clang::TemplateDecl>(text_decl))
      storage_decl = td->getTemplatedDecl();
    bool extern_spec = false;
    if (auto *vd = llvm::dyn_cast_or_null<clang::VarDecl>(storage_decl))
      extern_spec = vd->getStorageClass() == clang::SC_Extern;
    else if (auto *fd =
                 llvm::dyn_cast_or_null<clang::FunctionDecl>(storage_decl))
      extern_spec = fd->getStorageClass() == clang::SC_Extern;
    // Only when this declaration is the thing carrying the linkage: under the
    // whole-body wrapping no `extern "C++"` is prefixed here, and the enclosing
    // block does not stand in for the keyword — [dcl.link]/7 treats only a
    // declaration DIRECTLY contained in a linkage-specification as `extern`,
    // and these are nested in their namespaces. Dropping it there would turn
    // `extern const char kMarker[];` into a definition of unknown size.
    if (extern_spec && !extern_cxx) strip_storage_extern(decl_text);
    // The declaration text (namespace wrappers added at emission time so
    // adjacent declarations in the same namespace can be merged into a single
    // `namespace ... { ... }` block).
    std::string block;
    if (extern_cxx)
      // The whole body already sits inside an extern "C++" block, so only the
      // export marker is left to add — and it is always owed, however the
      // entity is defined. Attaching a declaration to the global module does
      // not by itself make it the SAME declaration everywhere: a module that
      // cannot see this one introduces its own, and the two never merge, so
      // every signature naming the entity mismatches across the boundary. The
      // export is what makes the declaration findable, and gcc takes the first
      // declaration of a name as the one that introduces it, so an unexported
      // one ahead of the definition leaves the entity unexported for good.
      block += std::format("{} {}\n", shared_export_macro, decl_text);
    else if (exported && !cross_fwd)
      // A forward declaration of an entity this module defines as a plain
      // module entity: export it normally.
      block += std::format("export {}\n", decl_text);
    else if (exported)
      // This module defines AND exports the entity, but its definition is
      // `extern "C++"` (the entity is also declared elsewhere). The
      // declaration must be exported too: [module.interface]/6 makes an
      // exported declaration preceded by a non-exported one ill-formed, and a
      // compiler that takes the first declaration as authoritative then never
      // exports the entity at all.
      block += std::format("export extern \"C++\" {}\n", decl_text);
    else
      // The entity is defined by another module, so this declaration only has
      // to merge with that definition; this module does not own it and must
      // not export it.
      block += std::format("extern \"C++\" {}\n", decl_text);
    fwd_decls->push_back({nss, std::move(block)});
    // The injected declaration's signature may reference types declared in
    // system headers (e.g. `::std::string ToString(...)`). Those headers
    // were only reachable transitively through the now-imported declaring
    // header, so the GMF would drop them; record them as used so the injected
    // declaration is well-formed.
    record_decl_type_headers(d);
  }

private:
  clang::ASTContext &ctx;
  std::vector<ModPoint> &mods;
  const std::regex &re;
  std::string_view export_macro;
  const std::vector<std::string> &reachable_fqns;
  std::vector<std::string> &internal_fqns;
  bool no_internal_filter = false;
  std::set<std::string> *used_headers = nullptr;
  const std::vector<std::string> &defined_fqns;
  std::vector<std::pair<std::vector<std::string>, std::string>> *fwd_decls;
  std::string shared_export_macro;
  std::string linkage_macro;
  std::string extern_decl_macro;
  std::string inline_macro;
  bool extern_cxx = false;
  std::string extern_cxx_macro;
  const std::vector<std::string> &fwd_declared_fqns;
  const std::set<std::string> &same_module_free_fqns;
  const std::set<std::string> *library_headers = nullptr;
  std::map<std::string, std::vector<ModPoint>> *external_macro_mods = nullptr;
  bool gcc_modules = false;

  // Record the system-header files of declarations referenced by `qt` into
  // used_headers, so a tool-injected forward declaration whose signature uses
  // them (e.g. `::std::string`) compiles against the GMF.
  void record_type_headers(clang::QualType qt) {
    if (!used_headers || qt.isNull()) return;
    auto &sm = ctx.getSourceManager();
    auto record_decl = [&](clang::Decl *d) {
      if (!d) return;
      auto loc = d->getLocation();
      if (!loc.isValid() || sm.isInMainFile(loc)) return;
      auto f = sm.getFilename(loc);
      if (f.empty() || !sm.isInSystemHeader(loc)) return;
      used_headers->insert(f.str());
    };
    // Peel references/pointers/arrays to the underlying named type. Only the
    // types re-exported by libtooling are named here; everything else is
    // handled via generic QualType predicates.
    qt = qt.getCanonicalType();
    if (qt.isNull()) return;
    if (qt->isBuiltinType()) return;
    if (qt->isReferenceType()) {
      record_type_headers(qt.getNonReferenceType());
      return;
    }
    if (qt->isPointerType() || qt->isArrayType()) {
      record_type_headers(qt->getPointeeType());
      return;
    }
    if (auto *tst = llvm::dyn_cast<clang::TemplateSpecializationType>(qt)) {
      if (auto *td = tst->getTemplateName().getAsTemplateDecl())
        record_decl(td);
      for (auto &arg : tst->template_arguments()) {
        if (arg.getKind() == clang::TemplateArgument::Type)
          record_type_headers(arg.getAsType());
      }
      return;
    }
    if (auto *tt = llvm::dyn_cast<clang::TypedefType>(qt)) {
      record_decl(tt->getDecl());
      record_type_headers(tt->getDecl()->getUnderlyingType());
      return;
    }
    if (qt->isRecordType() || qt->isEnumeralType()) {
      if (auto *tag = qt->getAsTagDecl()) record_decl(tag);
    }
  }

  void record_decl_type_headers(clang::Decl *d) {
    auto *fd = llvm::dyn_cast<clang::FunctionDecl>(d);
    if (auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(d))
      fd = ft->getTemplatedDecl();
    if (fd) {
      record_type_headers(fd->getReturnType());
      for (auto *p : fd->parameters())
        if (p) record_type_headers(p->getType());
    } else if (auto *vd = llvm::dyn_cast<clang::VarDecl>(d)) {
      record_type_headers(vd->getType());
    }
  }

  // True when the FQN contains an internal segment (`internal`, `impl`, ...).
  bool is_internal_fqn(const std::string &fqn) {
    auto parts = split_on(fqn, ':');
    if (parts.size() < 2) return false;
    return std::ranges::any_of(
        std::views::take(parts, parts.size() - 1),
        [&](const std::string &seg) { return std::regex_match(seg, re); });
  }

  // True when the defining module exports `fqn`, so this module sees it via
  // its import of that module and no injected declaration is needed. Public
  // entities are always exported; internal entities are exported only when
  // listed as reachable (macro-reachable or via --reachable-fqn).
  bool visible_via_import(const std::string &fqn) {
    // With export-all (no_internal_filter) every library entity is exported, so
    // any entity whose defining module is imported is visible via that import.
    if (no_internal_filter) return true;
    if (!is_internal_fqn(fqn)) return true;  // public namespace → exported
    // Internal: exported only when listed as reachable.
    return std::ranges::any_of(reachable_fqns, [&](const std::string &r) {
      return matches_reachable(fqn, r, /*symmetric=*/true);
    });
  }

  // True when an entity with NO visible definition in this TU is nevertheless
  // exported by its (imported) defining module. A bare declaration whose
  // complete definition lives in a different library header is kept private
  // (kKeepPrivate) by the declaring module even when reachable — the
  // definition is what that module exports, and it is not imported here
  // (e.g. `TraceImpl` is declared in mylib-internal.h but defined in
  // mylib-internal-inl.h). Only entities whose defining module exports the
  // declaration (public-namespace, or reachable internal) are visible via the
  // import.
  bool is_exported_via_import(const std::string &fqn) {
    if (no_internal_filter) return true;
    if (std::ranges::any_of(defined_fqns, [&](const std::string &d) {
          return matches_reachable(fqn, d, /*symmetric=*/true);
        }))
      return false;  // kKeepPrivate: definition in another module
    // Public-namespace entities are exported by their defining module; internal
    // ones only when listed as reachable.
    if (!is_internal_fqn(fqn)) return true;
    return std::ranges::any_of(reachable_fqns, [&](const std::string &r) {
      return matches_reachable(fqn, r, /*symmetric=*/true);
    });
  }
};
