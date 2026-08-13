module;

export module modulizer.rewrite_inject;
import modulizer.astutil;
import modulizer.rewrite_util;
import modulizer.util;
import libtooling;
import std;

// Forward-declaration injection machinery: finds library entities this module
// references that are only forward-declared in an included library header, and
// indexes all namespace-scope library entities so declarations can be
// reproduced for cross-module entities.

// Collects references from the main file to library entities that are only
// forward-declared in an included library header. A module whose code uses such
// an entity cannot see it through an import (the declaring module keeps the
// forward declaration private), so the module needs its own copy. This mirrors
// the textual-include world, where the forward declaration was visible anywhere
// the declaring header was included.
export class FwdDeclRefVisitor : public clang::RecursiveASTVisitor<FwdDeclRefVisitor> {
public:
  FwdDeclRefVisitor(const clang::SourceManager &sm,
                    const std::set<std::string> &defined_fqns,
                    const std::set<std::string> &cross_module_fqns,
                    std::map<std::string, clang::NamedDecl *> &needed,
                    std::vector<std::string> &order,
                    std::set<std::string> &complete_needed)
      : sm(sm), defined_fqns(defined_fqns), cross_module_fqns(cross_module_fqns),
        needed(needed), order(order), complete_needed(complete_needed) {}

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  // Nonzero while scanning the definition of an already-recorded cross-module
  // entity. Dependencies discovered there (e.g. `IsConvertible`
  // → `IsConvertibleImpl`) must be recorded even though they
  // are not in the initial `cross_module_fqns` set.
  int scan_depth_ = 0;

  // Only namespace-scope library entities are injectable. Template parameters
  // (`kFromKind`), enum constants (`kBool`, `kInteger`), function parameters
  // and locals must never be recorded, even when found while scanning an
  // injected entity's definition.
  void record(clang::NamedDecl *d) {
    if (!d) return;
    auto *canon = llvm::dyn_cast_or_null<clang::NamedDecl>(d->getCanonicalDecl());
    if (!canon) return;
    // Skip template instantiations: their (missing) body makes them look like
    // forward declarations, but the defining template is what matters.
    if (auto *fd = llvm::dyn_cast<clang::FunctionDecl>(canon))
      if (fd->isTemplateInstantiation()) return;
    if (auto *rd = llvm::dyn_cast<clang::CXXRecordDecl>(canon))
      if (rd->getTemplateInstantiationPattern()) return;
    auto loc = canon->getLocation();
    if (!loc.isValid() || sm.isInMainFile(loc) || sm.isInSystemHeader(loc))
      return;
    auto fqn = canon->getQualifiedNameAsString();
    // Cross-module template-body-referenced entities (defined in ANOTHER
    // header but used inside this module's template bodies) must be recorded
    // even though they are fully defined: the consumer instantiates the
    // template at its point of definition, so this module needs a module-local
    // `extern "C++"` declaration (and the defining module marks the definition
    // `extern "C++"`). This is the `Matches` helper referenced from a member
    // function template's body case.
    if (cross_module_fqns.count(fqn)) {
      if (needed.count(fqn)) return;
      needed[fqn] = canon;
      // Transitive expansion: an injected entity's definition references other
      // cross-module entities (e.g. alias template
      // `IsConvertible = IsConvertibleImpl<...>`).
      // Those are recorded by the analyzer's own transitive scan, so they are
      // also in `cross_module_fqns`; record them (before this entity) so the
      // emitted blocks come out in dependency order.
      scan_definition(canon);
      order.push_back(fqn);
      return;
    }
    // Only forward declarations (no body / incomplete) of library entities.
    bool is_fwd = false;
    if (auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(canon)) {
      is_fwd = !ft->getTemplatedDecl()->doesThisDeclarationHaveABody();
    } else if (auto *fd = llvm::dyn_cast<clang::FunctionDecl>(canon)) {
      is_fwd = !fd->doesThisDeclarationHaveABody();
    } else if (auto *ct = llvm::dyn_cast<clang::ClassTemplateDecl>(canon)) {
      is_fwd = !ct->getTemplatedDecl()->isCompleteDefinition();
    } else if (auto *rd = llvm::dyn_cast<clang::CXXRecordDecl>(canon)) {
      is_fwd = !rd->isCompleteDefinition();
    }
    if (!is_fwd) {
      // A COMPLETE library type referenced by an injected entity's signature
      // (e.g. `faketype` in `operator==(faketype, faketype)`) is needed in full
      // — a forward declaration is insufficient for by-value parameters. Only
      // collected while scanning an injected cross-module entity's definition.
      if (scan_depth_ > 0 &&
          (llvm::isa<clang::CXXRecordDecl>(canon) ||
           llvm::isa<clang::ClassTemplateDecl>(canon)) &&
          !needed.count(fqn) && !complete_needed.count(fqn)) {
        complete_needed.insert(fqn);
        needed[fqn] = canon;
        order.push_back(fqn);
      }
      return;
    }
    if (!defined_fqns.count(fqn)) return;
    if (!needed.count(fqn)) needed[fqn] = canon;
  }

  bool in_main(clang::SourceLocation loc) const {
    return loc.isValid() && sm.isInMainFile(loc);
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
    if (e && in_main(e->getLocation()))
      record(llvm::dyn_cast_or_null<clang::NamedDecl>(e->getDecl()));
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *e) {
    if (e && in_main(e->getMemberLoc()))
      record(llvm::dyn_cast_or_null<clang::NamedDecl>(e->getMemberDecl()));
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *e) {
    if (e && in_main(e->getExprLoc()))
      record(llvm::dyn_cast_or_null<clang::NamedDecl>(e->getCalleeDecl()));
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *e) {
    if (e && in_main(e->getExprLoc()))
      record(llvm::dyn_cast_or_null<clang::NamedDecl>(e->getConstructor()));
    return true;
  }

  bool VisitUnresolvedLookupExpr(clang::UnresolvedLookupExpr *e) {
    if (!e || !in_main(e->getExprLoc())) return true;
    for (auto *d : e->decls())
      record(llvm::dyn_cast_or_null<clang::NamedDecl>(d));
    return true;
  }

  bool VisitUnresolvedMemberExpr(clang::UnresolvedMemberExpr *e) {
    if (!e || !in_main(e->getMemberLoc())) return true;
    for (auto *d : e->decls())
      record(llvm::dyn_cast_or_null<clang::NamedDecl>(d));
    return true;
  }

  bool VisitTypeLoc(clang::TypeLoc tl) {
    if (!tl || !in_main(tl.getBeginLoc())) return true;
    auto *t = tl.getTypePtr();
    if (!t) return true;
    if (auto *td = t->getAsTagDecl()) { record(td); return true; }
    if (auto *tst = llvm::dyn_cast<clang::TemplateSpecializationType>(t)) {
      if (auto *td = tst->getTemplateName().getAsTemplateDecl())
        record(td);
    }
    return true;
  }

private:
  const clang::SourceManager &sm;
  const std::set<std::string> &defined_fqns;
  const std::set<std::string> &cross_module_fqns;
  std::map<std::string, clang::NamedDecl *> &needed;
  std::vector<std::string> &order;
  std::set<std::string> &complete_needed;

  // Scan the definition of a recorded cross-module entity and record every
  // other cross-module library entity it references. Alias templates copy
  // their whole RHS into the using module, and function templates keep their
  // trailing return type (which may reference other entities, e.g. `auto
  // call(...) -> decltype(apply_impl(...))`). Both must have their
  // dependencies injected too. Function/class BODY references are not needed
  // (the injection strips the body). Rather than chase the AST through
  // dependent decltype types, scan the declaration source text for `ns::Name`
  // sequences.
  void scan_definition(clang::NamedDecl *d) {
    if (!d) return;
    ++scan_depth_;
    // Aliases copy their whole RHS; traverse the AST so unqualified
    // same-namespace references (e.g. `using X = Impl<T>;`) resolve to the
    // cross-module entity.
    if (auto *tat = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(d)) {
      DeclRefScanner<FwdDeclRefVisitor> scanner(*this);
      if (auto *ta = tat->getTemplatedDecl()) scanner.TraverseDecl(ta);
    } else if (auto *ta = llvm::dyn_cast<clang::TypeAliasDecl>(d)) {
      DeclRefScanner<FwdDeclRefVisitor> scanner(*this);
      scanner.TraverseDecl(ta);
    } else if (auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(d)) {
      // The injected declaration keeps the signature including the trailing
      // return type (`auto f(...) -> decltype(...)`), which may reference
      // helper entities like ApplyImpl. Traverse the return type's underlying
      // decltype expression via the AST, then scan the declaration source text
      // for `ns::Name` sequences.
      if (auto *fd = ft->getTemplatedDecl()) {
        // Signature parameter/return types may name COMPLETE library types
        // taken by value (`bool equal(Baz lhs, ...)`), which must be injected
        // in full.
        DeclRefScanner<FwdDeclRefVisitor> tscanner(*this);
        tscanner.TraverseDecl(fd);
        if (auto *rt = llvm::dyn_cast_or_null<clang::DecltypeType>(
                fd->getReturnType().getTypePtrOrNull())) {
          CallRefScanner<FwdDeclRefVisitor> scanner(*this);
          scanner.TraverseStmt(rt->getUnderlyingExpr());
        }
      }
      // Scan the declaration source text for `ns::Name` sequences.
      std::string text = clang::Lexer::getSourceText(
          clang::CharSourceRange::getTokenRange(ft->getSourceRange()), sm,
          clang::LangOptions())
                             .str();
      for (auto &fqn : scan_fqn_text(text)) {
        if (cross_module_fqns.count(fqn) && !needed.count(fqn)) {
          needed[fqn] = nullptr;
          order.push_back(fqn);
        }
      }
    } else if (auto *fd = llvm::dyn_cast<clang::FunctionDecl>(d)) {
      // A plain function keeps its full signature when injected (e.g.
      // `Info* make_and_register_test_info(...)`), so library entities named by
      // its return/parameter types (here `Info`) must be declared before it
      // in this module. Record them so they are injected in dependency order.
      DeclRefScanner<FwdDeclRefVisitor> scanner(*this);
      scanner.TraverseDecl(fd);
    }
    --scan_depth_;
  }
};

// Builds an FQN -> NamedDecl index of all namespace-scope library entities in
// the TU, so the rewriter can inject a declaration for any cross-module entity
// named in `fwd_declared_fqns` even when it is not directly referenced by the
// main file's template bodies (e.g. `apply_impl`, which is only referenced from
// `call`'s trailing return type in another header).
export class DeclIndexVisitor
    : public clang::RecursiveASTVisitor<DeclIndexVisitor> {
public:
  DeclIndexVisitor(const clang::SourceManager &sm,
                   std::map<std::string, clang::NamedDecl *> &index)
      : sm(sm), index(index) {}

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  void add(clang::NamedDecl *d) {
    if (!d || d->isImplicit()) return;
    if (!d->getIdentifier()) return;
    auto loc = d->getLocation();
    if (!loc.isValid() || sm.isInMainFile(loc) || sm.isInSystemHeader(loc))
      return;
    // Only namespace-scope entities.
    if (!is_namespace_scope(d)) return;
    auto fqn = d->getQualifiedNameAsString();
    if (!index.count(fqn)) index[fqn] = d;
  }

  bool VisitFunctionTemplateDecl(clang::FunctionTemplateDecl *ft) {
    add(ft);
    return true;
  }
  bool VisitFunctionDecl(clang::FunctionDecl *fd) { add(fd); return true; }
  bool VisitCXXRecordDecl(clang::CXXRecordDecl *rd) { add(rd); return true; }
  bool VisitClassTemplateDecl(clang::ClassTemplateDecl *ct) {
    add(ct);
    return true;
  }
  bool VisitTypeAliasTemplateDecl(clang::TypeAliasTemplateDecl *tat) {
    add(tat);
    return true;
  }
  bool VisitTypeAliasDecl(clang::TypeAliasDecl *ta) { add(ta); return true; }
  bool VisitEnumDecl(clang::EnumDecl *ed) { add(ed); return true; }
  bool VisitVarDecl(clang::VarDecl *vd) { add(vd); return true; }

private:
  const clang::SourceManager &sm;
  std::map<std::string, clang::NamedDecl *> &index;
};
