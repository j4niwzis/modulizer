module;

export module modulizer.cross_module;
import modulizer.analyzer;
import modulizer.astutil;
import modulizer.util;
import libtooling;
import std;

// Cross-module entity analysis: FQN sets of entities whose declarations or
// definitions span multiple library headers / implementation files, which
// determines where `extern "C++"` shared-entity wrapping is needed.

// FQNs of entities that are forward-declared in one library header and
// completely defined in a *different* one. Such entities are declared in
// modules that do not define them, so every declaration (and the defining
// module's definition) must be `extern "C++"` to keep a single shared entity
// across module boundaries.
// `precomputed`, when given, is the per-file entity model set for `paths`
// (see analyze_files_per_file): this analysis needs nothing else from a parse,
// so a caller that already extracted them must not pay for a second sweep.
export std::vector<std::string> cross_module_fwd_declared_fqns(
    const std::vector<std::string> &paths,
    const std::vector<std::string> &extra_args,
    const std::vector<EntityModel> *precomputed = nullptr) {
  std::vector<std::map<std::string, std::set<std::string>>> complete_partials(
      paths.size());
  std::vector<std::map<std::string, std::set<std::string>>> fwd_partials(
      paths.size());
  std::vector<EntityModel> owned;
  if (!precomputed) owned.resize(paths.size());
  std::vector<std::vector<std::string>> ifdef_macros(paths.size());
  if (!precomputed)
    parallel_parse(
        paths, extra_args, /*delayed_template_parsing=*/false,
        [&](std::size_t i, const std::string &) {
          return std::make_unique<EntityExtractionFactory>(owned[i],
                                                           ifdef_macros[i]);
        });
  const std::vector<EntityModel> &models = precomputed ? *precomputed : owned;
  parallel_for(paths.size(), [&](std::size_t i) {
        const std::string &path = paths[i];
        auto &complete_files = complete_partials[i];
        auto &fwd_files = fwd_partials[i];
        for (auto &item : models[i].items) {
          auto fqn = fqn_of(item.ns_path, item.name);
          if (item.complete)
            complete_files[fqn].insert(path);
          else
            fwd_files[fqn].insert(path);
        }
      });
  std::map<std::string, std::set<std::string>> complete_files, fwd_files;
  for (std::size_t i = 0; i < paths.size(); ++i) {
    for (auto &[fqn, files] : complete_partials[i])
      complete_files[fqn].insert_range(files);
    for (auto &[fqn, files] : fwd_partials[i])
      fwd_files[fqn].insert_range(files);
  }
  std::vector<std::string> out;
  for (auto &[fqn, files] : fwd_files) {
    auto it = complete_files.find(fqn);
    if (it == complete_files.end()) {
      // Declared by the library, defined by none of its headers: whatever
      // defines it does so outside the module — an implementation unit, or a
      // consumer (gtest friend-declares `internal::TestEventListenersAccessor`
      // so its own unit test can define it and reach the private members).
      // That definition is in the global module, so this declaration has to be
      // `extern "C++"` too; a module-attached declaration would make the two a
      // different entity ("declaration in the global module follows
      // declaration in module X"). A type that is never defined anywhere is
      // unaffected in practice — nothing can conflict with it.
      out.push_back(fqn);
      continue;
    }
    // A name defined by more than one header is a set of overloads split
    // across files, and a forward declaration of it names whichever overload
    // lives elsewhere — the declaring file defining an overload of its own does
    // not make the pair local. The declaration side (fwd_action) decides by
    // name as well, so this side has to be just as coarse or the two disagree,
    // leaving a global-module declaration against a module-attached definition.
    bool cross = it->second.size() > 1;
    for (auto &f : files)
      if (!it->second.count(f)) cross = true;  // declared where it is not defined
    if (cross) out.push_back(fqn);
  }
  return out;
}

// FQNs of entities that are *referenced* inside template bodies in one library
// header but defined in a *different* one. A consumer instantiating those
// templates (e.g. a macro expansion invoking an internal helper
// `MatchHelper(args)`) resolves the name at the point of
// definition in the USING module, so that module must be able to see a
// declaration. The entity is typically internal and not exported, so the using
// module needs an injected module-local `extern "C++"` declaration and the
// defining module must mark the definition `extern "C++"` — the same
// shared-entity treatment as cross-module forward-declared entities.
// Result of cross-module template-body analysis. `fwd_declared` holds
// forward-declarable entities (function/class templates, functions, classes)
// that the using module must re-declare via a module-local `extern "C++"`
// declaration. `aliases` holds ALIAS templates (which cannot be forward
// declared) — those must be EXPORTED from their defining module instead, so
// the using module sees them via its import.
export struct TemplateBodyRefResult {
  std::vector<std::string> fwd_declared;
  std::vector<std::string> aliases;
  // Internal entities another module of this library references outside a
  // template body. Their defining module must EXPORT them: the name resolves
  // where the using module is compiled, so the import has to carry it.
  std::vector<std::string> referenced;
  // Every friend declaration seen while scanning, as (enclosing class FQN,
  // friend target FQN). friend_extern_fqns keeps the targets whose enclosing
  // class is `extern "C++"`, which is decided later — the scan itself does not
  // depend on that, so it rides along here instead of costing its own sweep.
  std::vector<std::pair<std::string, std::string>> friend_pairs;
};

// Records every friend declaration as (enclosing class FQN, friend target
// FQN). Which of them matter depends on the set of `extern "C++"` entities,
// which is only complete later; the scan does not, so it can ride on a sweep
// that is happening anyway and be filtered afterwards.
class FriendPairCollector
    : public clang::RecursiveASTVisitor<FriendPairCollector> {
public:
  FriendPairCollector(const clang::SourceManager &sm,
                      std::vector<std::pair<std::string, std::string>> &out)
      : sm(sm), out(out) {}

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  bool VisitFriendDecl(clang::FriendDecl *f) {
    if (!f) return true;
    auto *cls = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(f->getDeclContext());
    if (!cls) return true;
    auto loc = cls->getLocation();
    if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
    auto enclosing = cls->getQualifiedNameAsString();
    if (auto *tsi = f->getFriendType())
      if (auto *td = tsi->getType()->getAsTagDecl())
        out.emplace_back(enclosing, td->getQualifiedNameAsString());
    if (auto *nd = llvm::dyn_cast_or_null<clang::NamedDecl>(f->getFriendDecl()))
      out.emplace_back(enclosing, nd->getQualifiedNameAsString());
    return true;
  }

private:
  const clang::SourceManager &sm;
  std::vector<std::pair<std::string, std::string>> &out;
};

// The raw per-file output of the template-body scan, before it is folded into
// a TemplateBodyRefResult. Kept separate so the scan can ride a sweep that is
// already parsing these files instead of running one of its own.
export struct TemplateBodyRawScan {
  std::vector<std::set<std::string>> outs;
  std::vector<std::set<std::string>> aliases;
  std::vector<std::set<std::string>> referenced;
  // Per header: the library entities it names that another header defines.
  // Kept per file, not folded, because the import belongs to that one header.
  std::vector<std::set<std::string>> needed;
  std::vector<std::vector<std::pair<std::string, std::string>>> friend_pairs;

  explicit TemplateBodyRawScan(std::size_t n = 0)
      : outs(n), aliases(n), referenced(n), needed(n), friend_pairs(n) {}
};

// Collects the FQNs an entity references from template bodies, restricted to
// entities defined in a different file.
class TemplateBodyRefCollector
    : public clang::RecursiveASTVisitor<TemplateBodyRefCollector> {
public:
  TemplateBodyRefCollector(
      const clang::SourceManager &sm,
      const std::map<std::string, std::set<std::string>> &defined_files,
      const std::set<std::string> &alias_fqns, const std::string &self_path,
      std::set<std::string> &out, std::set<std::string> &out_aliases,
      std::set<std::string> &out_referenced, std::set<std::string> &out_needed)
      : sm(sm), defined_files(defined_files), alias_fqns(alias_fqns),
        self_path(self_path), out(out), out_aliases(out_aliases),
        out_referenced(out_referenced), out_needed(out_needed) {}

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  // Nonzero while traversing a template body (a function template, a class
  // template's member function, etc.). Only references made from inside such
  // bodies get instantiated at the consumer and need the shared-entity
  // treatment; references in plain (non-template) declarations resolve when
  // the module is compiled and are handled by normal export rules.
  int template_depth_ = 0;

  bool TraverseFunctionTemplateDecl(clang::FunctionTemplateDecl *d) {
    ++template_depth_;
    bool r = clang::RecursiveASTVisitor<TemplateBodyRefCollector>::
        TraverseFunctionTemplateDecl(d);
    --template_depth_;
    return r;
  }

  bool TraverseCXXMethodDecl(clang::CXXMethodDecl *d) {
    // Member functions of a class template (pattern) are instantiated at the
    // consumer too.
    if (d && d->getParent() &&
        d->getParent()->getDescribedClassTemplate())
      ++template_depth_;
    bool r = clang::RecursiveASTVisitor<TemplateBodyRefCollector>::
        TraverseCXXMethodDecl(d);
    if (d && d->getParent() &&
        d->getParent()->getDescribedClassTemplate())
      --template_depth_;
    return r;
  }

  // The FQN of an internal-namespace entity that another library header
  // defines, or nothing when this reference is not one of those.
  std::optional<std::string> cross_module_internal_fqn(clang::NamedDecl *d) {
    if (!d) return std::nullopt;
    auto *canon = llvm::dyn_cast_or_null<clang::NamedDecl>(d->getCanonicalDecl());
    if (!canon) return std::nullopt;
    auto loc = canon->getLocation();
    if (!loc.isValid() || sm.isInMainFile(loc) || sm.isInSystemHeader(loc))
      return std::nullopt;
    // Only namespace-scope entities are injectable. Skip template parameters
    // (`kFromKind`), enum constants (`kBool`, `kInteger`), locals, etc.
    {
      bool ns_scope = false;
      for (auto *dc = canon->getDeclContext(); dc; dc = dc->getParent()) {
        if (llvm::isa<clang::NamespaceDecl>(dc) ||
            llvm::isa<clang::TranslationUnitDecl>(dc)) {
          ns_scope = true;
          break;
        }
        if (llvm::isa<clang::FunctionDecl>(dc) ||
            llvm::isa<clang::CXXRecordDecl>(dc) ||
            llvm::isa<clang::ClassTemplateDecl>(dc) ||
            llvm::isa<clang::FunctionTemplateDecl>(dc) ||
            llvm::isa<clang::EnumDecl>(dc) ||
            llvm::isa<clang::TypeAliasTemplateDecl>(dc) ||
            llvm::isa<clang::TypeAliasDecl>(dc))
          break;
      }
      if (!ns_scope) return std::nullopt;
    }
    auto fqn = canon->getQualifiedNameAsString();
    // Only INTERNAL-namespace entities need the shared-entity treatment:
    // public entities are exported by default and thus visible to the using
    // module via its import, so no injected declaration is needed. The
    // real-world case is `lib::internal::MatchHelper` used inside
    // `Spec<F>::Call` — `lib::PublicValue` (public) must not
    // be flagged.
    if (!fqn.contains("::internal::") && !fqn.contains("::detail::") &&
        !fqn.contains("::impl::")) return std::nullopt;
    auto it = defined_files.find(fqn);
    if (it == defined_files.end()) return std::nullopt;  // not a library entity
    if (it->second.count(self_path)) return std::nullopt;  // defined here
    return fqn;
  }

  // A reference from inside a template body: instantiated at the consumer, so
  // the name resolves there and the entity needs the shared-entity treatment.
  void record(clang::NamedDecl *d) {
    auto found = cross_module_internal_fqn(d);
    if (!found) return;
    auto &fqn = *found;
    // Alias templates cannot be forward-declared: mark them for EXPORT from
    // the defining module (visible via import) instead of injecting a copy.
    if (alias_fqns.count(fqn)) {
      out_aliases.insert(fqn);
      return;
    }
    if (!out.insert(fqn).second) return;  // already recorded
    // Transitive expansion: an injected entity's own definition may reference
    // further cross-module entities. For example
    // `IsConvertible<From, To>` (defined in
    // internal-utils.h) is `using IsConvertible =
    // IsConvertibleImpl<...>` — the impl alias must be
    // injected too, or the using module's copied definition cannot compile.
    // Recursively scan the definition and record every cross-module entity it
    // mentions.
    scan_definition(llvm::dyn_cast_or_null<clang::NamedDecl>(
        d->getCanonicalDecl()));
  }

  // Scan the definition of a recorded cross-module entity and record every
  // library entity it references (transitively). Alias templates copy their
  // whole RHS; function templates keep their trailing return type (which may
  // reference other entities, e.g. `auto call(...) ->
  // decltype(apply_impl(...))`); both must have their dependencies recorded.
  // Function/class BODY references are not needed (the injection strips the
  // body).
  void scan_definition(clang::NamedDecl *d) {
    if (!d) return;
    if (auto *tat = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(d)) {
      DeclRefScanner<TemplateBodyRefCollector> scanner(*this);
      if (auto *ta = tat->getTemplatedDecl()) scanner.TraverseDecl(ta);
    } else if (auto *ta = llvm::dyn_cast<clang::TypeAliasDecl>(d)) {
      DeclRefScanner<TemplateBodyRefCollector> scanner(*this);
      scanner.TraverseDecl(ta);
    } else if (auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(d)) {
      // The injected declaration keeps the trailing return type (`auto
      // f(...) -> decltype(...)`), which may reference helper entities like
      // ApplyImpl. Traverse the return type's underlying decltype expression
      // via the AST, then fall back to scanning the declaration source text
      // for qualified names.
      if (auto *fd = ft->getTemplatedDecl()) {
        if (auto *rt = llvm::dyn_cast_or_null<clang::DecltypeType>(
                fd->getReturnType().getTypePtrOrNull())) {
          CallRefScanner<TemplateBodyRefCollector> scanner(*this);
          scanner.TraverseStmt(rt->getUnderlyingExpr());
        }
      }
      // Fall back / supplement: scan the declaration source text for
      // `ns::Name` sequences.
      std::string text = clang::Lexer::getSourceText(
          clang::CharSourceRange::getTokenRange(ft->getSourceRange()), sm,
          clang::LangOptions())
                             .str();
      for (auto &fqn : scan_fqn_text(text)) {
        auto it = defined_files.find(fqn);
        if (it != defined_files.end() &&
            !it->second.count(self_path) &&
            fqn.contains("::internal::")) {
          if (alias_fqns.count(fqn))
            out_aliases.insert(fqn);
          else
            out.insert(fqn);
        }
      }
    }
  }

  // A reference made ANYWHERE in this header, template body or not. Outside a
  // template the name resolves where this module is compiled, so the entity
  // has to be visible here — and for an internal-namespace entity that means
  // its own module must export it. Nothing is injected: a copy would be a
  // second entity beside the one the import already carries. A library whose
  // modules use each other's `detail::` helpers is otherwise left with
  //
  //   error: declaration of 'X' must be imported from module 'lib.other'
  //          before it is required
  //
  // because nothing else looks for this: one analysis wants forward
  // declarations, the other only looks inside template bodies.
  void record_referenced(clang::NamedDecl *d) {
    if (auto fqn = cross_module_internal_fqn(d)) out_referenced.insert(*fqn);
    record_needed(d);
  }

  // Every entity of this library that this header names and another header
  // defines, public ones included. A header that names one without including
  // the header that defines it is not self-contained: it compiles only where
  // something else was included first, which a module unit cannot rely on —
  //
  //   error: calling 'f' with incomplete return type 'lib::Thing'
  //
  // The module that defines it has to be imported, and only this can say so:
  // there is no include to derive it from.
  void record_needed(clang::NamedDecl *d) {
    if (!d) return;
    auto *canon = llvm::dyn_cast_or_null<clang::NamedDecl>(d->getCanonicalDecl());
    if (!canon) return;
    auto loc = canon->getLocation();
    if (!loc.isValid() || sm.isInMainFile(loc) || sm.isInSystemHeader(loc))
      return;
    auto fqn = canon->getQualifiedNameAsString();
    auto it = defined_files.find(fqn);
    if (it == defined_files.end()) return;
    if (it->second.count(self_path)) return;
    out_needed.insert(fqn);
  }

  // A call names its function, not the types in its signature — the return type
  // of `x.f()` is written in the header that declares `f`, not here. Yet using
  // the result needs that type COMPLETE, so the module defining it has to be
  // imported as well:
  //
  //   error: calling 'f' with incomplete return type 'lib::Thing'
  //
  // Asked of every callee, not only of one that is itself a library entity: a
  // MEMBER function is not one (only namespace-scope entities are), and it is
  // exactly a member whose return type goes missing this way.
  void record_signature_types(clang::FunctionDecl *fd) {
    if (!fd) return;
    record_type(fd->getReturnType());
    for (auto *pd : fd->parameters()) record_type(pd->getType());
  }

  // Record the entity a type names, looking through pointers, references and
  // template arguments.
  void record_type(clang::QualType t) {
    if (t.isNull()) return;
    t = t.getNonReferenceType();
    while (!t.isNull() && (t->isPointerType() || t->isArrayType())) {
      auto next = t->getPointeeType();
      if (next.isNull()) next = t->getAsArrayTypeUnsafe()
                                    ? t->getAsArrayTypeUnsafe()->getElementType()
                                    : clang::QualType();
      if (next.isNull()) break;
      t = next;
    }
    if (t.isNull()) return;
    if (auto *td = t->getAsTagDecl()) record_needed(td);
    if (auto *tst = t->getAs<clang::TemplateSpecializationType>()) {
      if (auto *td = tst->getTemplateName().getAsTemplateDecl())
        record_needed(td);
      for (auto &arg : tst->template_arguments())
        if (arg.getKind() == clang::TemplateArgument::Type)
          record_type(arg.getAsType());
    }
  }

  bool in_main(clang::SourceLocation loc) const {
    return loc.isValid() && sm.isInMainFile(loc);
  }
  bool VisitCallExpr(clang::CallExpr *e) {
    if (!e || !in_main(e->getExprLoc())) return true;
    // The type of the call itself — what the result IS. Reaching it through
    // the callee's declaration does not always work (a member function is not
    // a library entity in its own right), and this is the value the code goes
    // on to use, so it is the one that has to be complete.
    record_type(e->getType());
    auto each = [&](clang::NamedDecl *d) {
      record_referenced(d);
      record_signature_types(llvm::dyn_cast_or_null<clang::FunctionDecl>(d));
      if (template_depth_ > 0) record(d);
    };
    if (auto *fd = e->getDirectCallee()) each(fd);
    else if (auto *e2 = e->getCallee()) {
      if (auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(e2))
        each(dre->getDecl());
      else if (auto *ule = llvm::dyn_cast<clang::UnresolvedLookupExpr>(e2))
        for (auto *d : ule->decls())
          each(llvm::dyn_cast_or_null<clang::NamedDecl>(d));
    }
    return true;
  }

  bool VisitUnresolvedLookupExpr(clang::UnresolvedLookupExpr *e) {
    if (!e || !in_main(e->getExprLoc())) return true;
    for (auto *d : e->decls()) {
      auto *nd = llvm::dyn_cast_or_null<clang::NamedDecl>(d);
      record_referenced(nd);
      if (template_depth_ > 0) record(nd);
    }
    return true;
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
    if (!e || !in_main(e->getLocation())) return true;
    record_referenced(e->getDecl());
    if (template_depth_ > 0) record(e->getDecl());
    return true;
  }

  bool VisitTypeLoc(clang::TypeLoc tl) {
    if (!tl || !in_main(tl.getBeginLoc())) return true;
    auto *t = tl.getTypePtr();
    if (!t) return true;
    auto each = [&](clang::NamedDecl *d) {
      record_referenced(d);
      if (template_depth_ > 0) record(d);
    };
    if (auto *td = t->getAsTagDecl()) { each(td); return true; }
    if (auto *tst = llvm::dyn_cast<clang::TemplateSpecializationType>(t)) {
      if (auto *td = tst->getTemplateName().getAsTemplateDecl()) each(td);
    }
    return true;
  }

private:
  const clang::SourceManager &sm;
  const std::map<std::string, std::set<std::string>> &defined_files;
  const std::set<std::string> &alias_fqns;
  const std::string &self_path;
  std::set<std::string> &out;
  std::set<std::string> &out_aliases;
  std::set<std::string> &out_referenced;
  std::set<std::string> &out_needed;
};

// Builds the consumers the template-body scan runs for one file, so it can be
// attached to any parse of that file. `defined_files`/`alias_fqns` come from
// the entity models, which a caller has before the sweep starts.
export std::unique_ptr<clang::ASTConsumer> make_template_body_scan_consumer(
    clang::CompilerInstance &ci,
    const std::map<std::string, std::set<std::string>> &defined_files,
    const std::set<std::string> &alias_fqns, const std::string &self_path,
    std::set<std::string> &out, std::set<std::string> &out_aliases,
    std::set<std::string> &out_referenced, std::set<std::string> &out_needed,
    std::vector<std::pair<std::string, std::string>> &friend_pairs) {
  struct Consumer : clang::ASTConsumer {
    Consumer(const std::map<std::string, std::set<std::string>> &df,
             const std::set<std::string> &af, std::string sp,
             std::set<std::string> &o, std::set<std::string> &oa,
             std::set<std::string> &orf, std::set<std::string> &on)
        : df(df), af(af), sp(std::move(sp)), o(o), oa(oa), orf(orf), on(on) {}
    void HandleTranslationUnit(clang::ASTContext &ctx) override {
      TemplateBodyRefCollector v(ctx.getSourceManager(), df, af, sp, o, oa, orf,
                                 on);
      v.TraverseDecl(ctx.getTranslationUnitDecl());
    }
    const std::map<std::string, std::set<std::string>> &df;
    const std::set<std::string> &af;
    std::string sp;
    std::set<std::string> &o;
    std::set<std::string> &oa;
    std::set<std::string> &orf;
    std::set<std::string> &on;
  };
  std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
  consumers.push_back(std::make_unique<Consumer>(
      defined_files, alias_fqns, self_path, out, out_aliases, out_referenced,
      out_needed));
  consumers.push_back(
      make_traverse_consumer<FriendPairCollector>(friend_pairs));
  (void)ci;
  return make_combined_consumer(std::move(consumers));
}

export TemplateBodyRefResult finalize_template_body_scan(
    const TemplateBodyRawScan &raw) {
  std::set<std::string> out, out_aliases, out_referenced;
  TemplateBodyRefResult result;
  for (std::size_t i = 0; i < raw.outs.size(); ++i) {
    out.insert_range(raw.outs[i]);
    out_aliases.insert_range(raw.aliases[i]);
    out_referenced.insert_range(raw.referenced[i]);
    result.friend_pairs.insert(result.friend_pairs.end(),
                               raw.friend_pairs[i].begin(),
                               raw.friend_pairs[i].end());
  }
  result.fwd_declared = std::ranges::to<std::vector>(out);
  result.aliases = std::ranges::to<std::vector>(out_aliases);
  result.referenced = std::ranges::to<std::vector>(out_referenced);
  return result;
}

// `precomputed` has the same meaning as in cross_module_fwd_declared_fqns:
// the first half of this analysis is the same entity extraction.
export TemplateBodyRefResult cross_module_template_body_referenced_fqns(
    const std::vector<std::string> &paths,
    const std::vector<std::string> &extra_args,
    const std::vector<EntityModel> *precomputed = nullptr) {
  std::vector<std::pair<std::map<std::string, std::set<std::string>>,
                        std::set<std::string>>> defined_partials(paths.size());
  std::vector<EntityModel> owned;
  if (!precomputed) owned.resize(paths.size());
  std::vector<std::vector<std::string>> ifdef_macros(paths.size());
  if (!precomputed)
    parallel_parse(
        paths, extra_args, /*delayed_template_parsing=*/false,
        [&](std::size_t i, const std::string &) {
          return std::make_unique<EntityExtractionFactory>(owned[i],
                                                           ifdef_macros[i]);
        });
  const std::vector<EntityModel> &models = precomputed ? *precomputed : owned;
  parallel_for(paths.size(), [&](std::size_t i) {
        const std::string &path = paths[i];
        auto &defined_files = defined_partials[i].first;
        auto &alias_fqns = defined_partials[i].second;
        for (auto &item : models[i].items) {
          if (!item.complete) continue;
          auto fqn = fqn_of(item.ns_path, item.name);
          defined_files[fqn].insert(path);
          if (item.kind == EntityItem::kAlias) alias_fqns.insert(fqn);
        }
      });
  std::map<std::string, std::set<std::string>> defined_files;
  std::set<std::string> alias_fqns;
  for (auto &partial : defined_partials) {
    for (auto &[fqn, files] : partial.first)
      defined_files[fqn].insert_range(files);
    alias_fqns.insert_range(partial.second);
  }
  // Collect the FQNs of entities referenced from template bodies in each
  // header, restricted to entities defined in a DIFFERENT header.
  std::set<std::string> out;
  std::set<std::string> out_aliases;

  std::set<std::string> out_referenced;
  std::vector<std::set<std::string>> partial_outs(paths.size());
  std::vector<std::set<std::string>> partial_aliases(paths.size());
  std::vector<std::set<std::string>> partial_referenced(paths.size());
  std::vector<std::set<std::string>> partial_needed(paths.size());
  std::vector<std::vector<std::pair<std::string, std::string>>> partial_friends(
      paths.size());
  parallel_parse(
      paths, extra_args, /*delayed_template_parsing=*/false,
      [&](std::size_t i, const std::string &path) {
        auto &out = partial_outs[i];
        auto &out_aliases = partial_aliases[i];
        auto &out_ref = partial_referenced[i];
        auto &out_need = partial_needed[i];
        auto &out_friends = partial_friends[i];
        return std::make_unique<VisitorFrontendActionFactory>(
            [&, self = path](clang::CompilerInstance &ci) {
              return make_template_body_scan_consumer(
                  ci, defined_files, alias_fqns, self, out, out_aliases,
                  out_ref, out_need, out_friends);
            });
      });
  for (std::size_t i = 0; i < paths.size(); ++i) {
    out.insert_range(partial_outs[i]);
    out_aliases.insert_range(partial_aliases[i]);
    out_referenced.insert_range(partial_referenced[i]);
  }
  TemplateBodyRefResult result;
  result.fwd_declared = std::ranges::to<std::vector>(out);
  result.aliases = std::ranges::to<std::vector>(out_aliases);
  result.referenced = std::ranges::to<std::vector>(out_referenced);
  for (auto &per_file : partial_friends)
    result.friend_pairs.insert(result.friend_pairs.end(), per_file.begin(),
                               per_file.end());
  return result;
}

// FQNs of classes that have out-of-line member-function definitions in the
// given source files. An implementation file lives in its own module; a member
// of a class can only be defined in the module that declares the class, so any
// such class must be `extern "C++"` (a global-module entity) for the member
// definition to be valid across modules.
export std::vector<std::string> out_of_line_member_class_fqns(
    const std::vector<std::string> &paths,
    const std::vector<std::string> &extra_args,
    const std::set<std::string> &same_module_stems = {}) {
  class MemberClassCollector
      : public clang::RecursiveASTVisitor<MemberClassCollector> {
  public:
    MemberClassCollector(const clang::SourceManager &sm,
                         std::set<std::string> &out,
                         const std::string &source_stem)
        : sm(sm), out(out), source_stem(source_stem) {}

    bool shouldVisitTemplateInstantiations() const { return false; }
    bool shouldVisitImplicitCode() const { return false; }

    static std::string file_stem(const clang::SourceManager &sm,
                                 clang::SourceLocation loc) {
      auto f = sm.getFilename(loc);
      if (f.empty()) return {};
      auto stem = f.str();
      auto dot = stem.rfind('.');
      if (dot != std::string::npos) stem = stem.substr(0, dot);
      auto slash = stem.rfind('/');
      if (slash != std::string::npos) stem = stem.substr(slash + 1);
      return stem;
    }

    // A class declared in a header whose stem matches the defining source's
    // stem is in the SAME module as that source (the source is its
    // implementation unit): no `extern "C++"` needed. A class declared in a
    // different header (e.g. a class declared in impl-inl.h but defined in
    // impl.cc) is cross-module and must
    // be extern "C++".
    bool class_is_same_module(clang::CXXRecordDecl *cls) {
      auto cls_loc = cls->getLocation();
      if (!cls_loc.isValid() || sm.isInMainFile(cls_loc)) return false;
      return !source_stem.empty() &&
             file_stem(sm, sm.getExpansionLoc(cls_loc)) == source_stem;
    }

    bool VisitFunctionDecl(clang::FunctionDecl *fd) {
      if (!fd || fd->isImplicit() || !fd->isThisDeclarationADefinition())
        return true;
      auto loc = fd->getLocation();
      if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
      if (auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
        if (auto *cls = llvm::dyn_cast<clang::CXXRecordDecl>(
                md->getDeclContext())) {
          // A class defined entirely in this implementation file is local to
          // the impl module: its member definitions need no extern "C++".
          auto cls_loc = cls->getLocation();
          if (cls_loc.isValid() && sm.isInMainFile(cls_loc)) return true;
          if (class_is_same_module(cls)) return true;
          out.insert(cls->getQualifiedNameAsString());
        }
      }
      return true;
    }

    bool VisitVarDecl(clang::VarDecl *vd) {
      if (!vd || vd->isImplicit() || !vd->hasInit()) return true;
      if (!vd->isStaticDataMember()) return true;
      auto loc = vd->getLocation();
      if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
      if (auto *cls = llvm::dyn_cast<clang::CXXRecordDecl>(
              vd->getDeclContext())) {
        auto cls_loc = cls->getLocation();
        if (cls_loc.isValid() && !sm.isInMainFile(cls_loc) &&
            !class_is_same_module(cls))
          out.insert(cls->getQualifiedNameAsString());
      }
      return true;
    }

  private:
    const clang::SourceManager &sm;
    std::set<std::string> &out;
    const std::string &source_stem;
  };

  std::vector<std::set<std::string>> partials(paths.size());
  parallel_parse(paths, extra_args, /*delayed_template_parsing=*/false,
                 [&](std::size_t i, const std::string &path) {
                   auto &out = partials[i];
                   auto source_stem =
                       std::filesystem::path(path).stem().string();
                   return std::make_unique<VisitorFrontendActionFactory>(
                       [&out, source_stem](clang::CompilerInstance &) {
                         return make_traverse_consumer<MemberClassCollector>(
                             out, source_stem);
                       });
                 });
  return merge_sets(partials);
}

// FQNs of namespace-scope free functions and variables defined in the given
// source files whose declaration comes from a library header (a different
// module). Such entities must be `extern "C++"` on both the header declaration
// and the implementation definition so they stay a single shared entity.
export std::vector<std::string> out_of_line_defined_free_fqns(
    const std::vector<std::string> &paths,
    const std::vector<std::string> &extra_args,
    bool collect_same_module = false) {
  class FreeEntityCollector
      : public clang::RecursiveASTVisitor<FreeEntityCollector> {
  public:
    FreeEntityCollector(const clang::SourceManager &sm,
                        std::set<std::string> &out,
                        const std::string &source_stem,
                        bool collect_same_module)
        : sm(sm), out(out), source_stem(source_stem),
          collect_same_module(collect_same_module) {}

    bool shouldVisitTemplateInstantiations() const { return false; }
    bool shouldVisitImplicitCode() const { return false; }

    bool VisitFunctionDecl(clang::FunctionDecl *fd) {
      if (!fd || fd->isImplicit() || !fd->isThisDeclarationADefinition())
        return true;
      if (llvm::dyn_cast<clang::CXXMethodDecl>(fd)) return true;
      auto loc = fd->getLocation();
      if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
      collect(fd);
      return true;
    }

    bool VisitVarDecl(clang::VarDecl *vd) {
      if (!vd || vd->isImplicit() || !vd->hasInit()) return true;
      if (!vd->isFileVarDecl()) return true;
      if (llvm::isa<clang::CXXRecordDecl>(vd->getDeclContext())) return true;
      if (llvm::isa<clang::FunctionDecl>(vd->getDeclContext())) return true;
      auto loc = vd->getLocation();
      if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
      collect(vd);
      return true;
    }

  private:
    void collect(clang::NamedDecl *d) {
      auto *cd = llvm::dyn_cast<clang::NamedDecl>(d->getCanonicalDecl());
      if (!cd) return;
      auto loc = sm.getExpansionLoc(cd->getLocation());
      // Declared in the impl file itself (e.g. a file-local helper) rather
      // than a library header: it is local to the impl module.
      if (!loc.isValid() || sm.isInMainFile(loc)) return;
      // A free entity declared in the header that this source implements (same
      // stem, e.g. a flag declared in `foo.h` and defined by `foo.cc`) lives in
      // the SAME module: its definition needs no `extern "C++"`. Only skip when
      // the declaring header's stem matches THIS source's stem; a different
      // stem (e.g. the flag is declared in `foo_impl.h` but defined in
      // `foo.cc`) is still cross-module.
      bool same_module = false;
      if (!source_stem.empty()) {
        auto stem = sm.getFilename(loc).str();
        auto dot = stem.rfind('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        auto slash = stem.rfind('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        same_module = (stem == source_stem);
      }
      if (same_module != collect_same_module) return;
      out.insert(cd->getQualifiedNameAsString());
    }

    const clang::SourceManager &sm;
    std::set<std::string> &out;
    const std::string &source_stem;
    const bool collect_same_module;
  };

  std::vector<std::set<std::string>> partials(paths.size());
  parallel_parse(paths, extra_args, /*delayed_template_parsing=*/false,
                 [&](std::size_t i, const std::string &path) {
                   auto &out = partials[i];
                   auto source_stem =
                       std::filesystem::path(path).stem().string();
                   return std::make_unique<VisitorFrontendActionFactory>(
                       [&out, source_stem,
                        collect_same_module](clang::CompilerInstance &) {
                         return make_traverse_consumer<FreeEntityCollector>(
                             out, source_stem, collect_same_module);
                       });
                 });
  return merge_sets(partials);
}

// FQNs of namespace-scope classes whose complete definition lives entirely in
// the given source files (not in any library header). Such classes are
// friend-declared in a library header (e.g. a result class friends an internal
// executor class) but defined in the impl unit: the friend declaration
// and the definition must be `extern "C++"` so they stay one shared entity and
// the friendship holds across modules.
export std::vector<std::string> out_of_line_defined_free_classes(
    const std::vector<std::string> &paths,
    const std::vector<std::string> &extra_args) {
  class FreeClassCollector
      : public clang::RecursiveASTVisitor<FreeClassCollector> {
  public:
    FreeClassCollector(const clang::SourceManager &sm,
                       std::set<std::string> &out)
        : sm(sm), out(out) {}

    bool shouldVisitTemplateInstantiations() const { return false; }
    bool shouldVisitImplicitCode() const { return false; }

    bool VisitCXXRecordDecl(clang::CXXRecordDecl *rd) {
      if (!rd || rd->isImplicit() || !rd->isCompleteDefinition()) return true;
      if (llvm::isa<clang::CXXRecordDecl>(rd->getDeclContext())) return true;
      if (llvm::isa<clang::FunctionDecl>(rd->getDeclContext())) return true;
      if (rd->getFriendObjectKind() != clang::Decl::FOK_None) return true;
      auto loc = rd->getLocation();
      if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
      // Only classes defined in the source file itself (no declaration in an
      // included library header) are candidates: those are the ones with no
      // visible definition when the header is analyzed alone.
      out.insert(rd->getQualifiedNameAsString());
      return true;
    }

  private:
    const clang::SourceManager &sm;
    std::set<std::string> &out;
  };

  std::vector<std::set<std::string>> partials(paths.size());
  parallel_parse(paths, extra_args, /*delayed_template_parsing=*/false,
                 [&](std::size_t i, const std::string &path) {
                   auto &out = partials[i];
                   return std::make_unique<VisitorFrontendActionFactory>(
                       [&out](clang::CompilerInstance &) {
                         return make_traverse_consumer<FreeClassCollector>(out);
                       });
                 });
  return merge_sets(partials);
}

// FQNs of entities that are friend-declared inside an `extern "C++"` class.
// A friend declaration inside a global-module class attaches to the global
// module, so the friend target's definition (wherever it lives) must also be
// `extern "C++"`. The impl side needs these FQNs to mark the definitions.
// The friend targets whose enclosing class is `extern "C++"`, from pairs a
// scan already collected — no parse of its own.
export std::vector<std::string> friend_extern_fqns_from_pairs(
    const std::vector<std::pair<std::string, std::string>> &friend_pairs,
    const std::vector<std::string> &fwd_declared_fqns) {
  std::set<std::string> fwd_set(fwd_declared_fqns.begin(),
                                fwd_declared_fqns.end());
  std::set<std::string> out;
  for (const auto &[enclosing, target] : friend_pairs)
    if (fwd_set.count(enclosing)) out.insert(target);
  return std::ranges::to<std::vector>(out);
}

export std::vector<std::string> friend_extern_fqns(
    const std::vector<std::string> &paths,
    const std::vector<std::string> &extra_args,
    const std::vector<std::string> &fwd_declared_fqns) {
  class FriendCollector
      : public clang::RecursiveASTVisitor<FriendCollector> {
  public:
    FriendCollector(const clang::SourceManager &sm,
                    const std::set<std::string> &fwd_set,
                    std::set<std::string> &out)
        : sm(sm), fwd_set(fwd_set), out(out) {}

    bool shouldVisitTemplateInstantiations() const { return false; }
    bool shouldVisitImplicitCode() const { return false; }

    bool VisitFriendDecl(clang::FriendDecl *f) {
      if (!f) return true;
      auto *cls =
          llvm::dyn_cast_or_null<clang::CXXRecordDecl>(f->getDeclContext());
      if (!cls) return true;
      auto loc = cls->getLocation();
      if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
      if (!fwd_set.count(cls->getQualifiedNameAsString())) return true;
      if (auto *tsi = f->getFriendType())
        if (auto *td = tsi->getType()->getAsTagDecl())
          out.insert(td->getQualifiedNameAsString());
      if (auto *nd =
              llvm::dyn_cast_or_null<clang::NamedDecl>(f->getFriendDecl()))
        out.insert(nd->getQualifiedNameAsString());
      return true;
    }

  private:
    const clang::SourceManager &sm;
    const std::set<std::string> &fwd_set;
    std::set<std::string> &out;
  };

  std::set<std::string> fwd_set(fwd_declared_fqns.begin(),
                                fwd_declared_fqns.end());
  std::vector<std::set<std::string>> partials(paths.size());
  parallel_parse(paths, extra_args, /*delayed_template_parsing=*/false,
                 [&](std::size_t i, const std::string &path) {
                   auto &out = partials[i];
                   return std::make_unique<VisitorFrontendActionFactory>(
                       [&fwd_set, &out](clang::CompilerInstance &) {
                         return make_traverse_consumer<FriendCollector>(
                             fwd_set, out);
                       });
                 });
  return merge_sets(partials);
}
