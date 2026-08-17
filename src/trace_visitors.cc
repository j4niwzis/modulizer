module;

export module modulizer.trace_visitors;
import modulizer.astutil;
import modulizer.rewrite_util;
import modulizer.util;
import libtooling;
import std;

// AST visitors used by consumer reachability tracing: internal-entity
// collection from producer headers, consumer reference finding, and the
// transitive alias closure. The trace orchestration (modulizer.consumer_trace)
// drives these via parallel_parse.

namespace {

// True when the CONSUMER's main file provides a definition for `d` — the
// library only forward-declares the entity and the consumer defines it (e.g.
// a library header fwd-declares `SuccessChecker` and the test defines it). The
// module must not export such an entity: exporting its forward declaration
// would collide with the consumer's own global-module definition.
bool consumer_defined_in_main(clang::NamedDecl *d,
                              const clang::SourceManager &sm) {
  if (!d) return false;
  clang::Decl *def = nullptr;
  if (auto *rd = llvm::dyn_cast<clang::CXXRecordDecl>(d))
    def = rd->getDefinition();
  else if (auto *fd = llvm::dyn_cast<clang::FunctionDecl>(d))
    def = fd->getDefinition();
  else if (auto *vd = llvm::dyn_cast<clang::VarDecl>(d))
    def = vd->getDefinition();
  if (!def) return false;
  return sm.isInMainFile(sm.getExpansionLoc(def->getLocation()));
}

// RAII recursion-depth guard shared by the visitors that walk consumer / type
// reference graphs; both consumers cap recursion with the same pattern.
struct ScopeDepth {
  int &depth;
  explicit ScopeDepth(int &depth) : depth(depth) { ++depth; }
  ~ScopeDepth() { --depth; }
};

}  // namespace

export class InternalCollector : public NsPathVisitor<InternalCollector> {
public:
  InternalCollector(const clang::SourceManager &sm, std::set<std::string> *out)
      : out(out), sm(sm),
        main_fid(sm.getMainFileID()) {}

  bool enabled() const { return out != nullptr; }

  bool is_internal_ns(const std::string &name) { return is_internal(name); }

  bool VisitNamedDecl(clang::NamedDecl *nd) {
    if (!nd || nd->isImplicit()) return true;
    if (internal_depth_ == 0) return true;
    auto loc = sm.getExpansionLoc(nd->getLocation());
    if (!loc.isValid() || sm.getFileID(loc) != main_fid) return true;

    out->insert(fqn_of(ns_path_, nd->getNameAsString()));
    return true;
  }

private:
  static bool is_internal(const std::string &name) {
    return name == "internal" || name == "detail" || name == "impl";
  }

  std::set<std::string> *out;
  const clang::SourceManager &sm;
  clang::FileID main_fid;
};

export class ConsumerRefFinder
    : public clang::RecursiveASTVisitor<ConsumerRefFinder> {
public:
  ConsumerRefFinder(const clang::SourceManager &sm,
                    const std::map<std::string, std::set<std::string>>
                        &producer_internal,
                    std::map<std::string, std::set<std::string>> &reachable,
                    const std::set<std::string> *producer_files = nullptr)
      : sm(sm),
        consumer_fid(sm.getMainFileID()),
        producer_internal(producer_internal),
        reachable(reachable),
        producer_files(producer_files) {}

  void setContext(clang::ASTContext &ctx) { ctx_ = &ctx; }

  // Whether a reference written at `loc` is a use BY the consumer. The main
  // file always is. Without a producer set that is all that counts, which
  // makes every reference in a header the consumer includes invisible: the
  // use of `internal::ElemFromList` in gmock's own
  // `gmock/internal/gmock-internal-utils.h` is as real as one in the .cc that
  // includes it, and gmock reaches gtest through the same imports. So with a
  // producer set, a reference counts unless it is written in one of the
  // headers being rewritten — a use inside the library itself is not a
  // consumer use, and counting those would export every internal entity.
  bool is_consumer_use(clang::SourceLocation loc) const {
    if (!loc.isValid()) return false;
    auto eloc = sm.getExpansionLoc(loc);
    if (sm.isInMainFile(eloc)) return true;
    if (!producer_files) return false;
    if (sm.isInSystemHeader(eloc)) return false;
    auto fe = sm.getFileEntryRefForID(sm.getFileID(eloc));
    if (!fe) return false;
    return !producer_files->count(fe->getName().str());
  }

  bool shouldVisitImplicitCode() const { return true; }
  bool shouldVisitTemplateInstantiations() const { return true; }

  using Base = clang::RecursiveASTVisitor<ConsumerRefFinder>;

  bool VisitDeclRefExpr(clang::DeclRefExpr *dre) {
    if (dre) {
      checkDecl(dre->getDecl());
      // A qualified reference (`foo<int>::value` resolves to the static member
      // `value`) carries the base TYPE in the nested-name-specifier; check it
      // so alias templates referenced this way are still exported.
      if (auto nns = dre->getQualifier())
        if (nns.getKind() == clang::NestedNameSpecifier::Kind::Type)
          if (auto *qt = nns.getAsType())
            checkType(clang::QualType(qt, 0));
      // The WRITTEN template arguments of a template-using expression (e.g.
      // `std::is_same_v<internal::box_type<int>, int>`) may name internal
      // aliases/types that are lost once the specialization desugars them.
      if (auto *args = dre->getTemplateArgs())
        for (unsigned i = 0; i < dre->getNumTemplateArgs(); ++i) {
          auto arg = args[i].getArgument();
          if (arg.getKind() == clang::TemplateArgument::Type)
            checkType(arg.getAsType());
          if (auto *tsi = args[i].getTypeSourceInfo())
            checkType(tsi->getType());
        }
    }
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *ce) {
    return checkDecl(ce ? ce->getDirectCallee() : nullptr);
  }

  bool VisitFunctionDecl(clang::FunctionDecl *fd) {
    if (!fd) return true;
    if (!is_consumer_use(fd->getLocation())) return true;
    checkType(fd->getReturnType());
    for (unsigned i = 0; i < fd->getNumParams(); ++i)
      checkType(fd->getParamDecl(i)->getType());
    // Check template parameter default arguments (SFINAE detection)
    if (auto *ft = fd->getCanonicalDecl()->getDescribedFunctionTemplate())
      if (auto *tpl = ft->getTemplateParameters())
        for (auto *p : *tpl)
          if (auto *ttp = llvm::dyn_cast<clang::TemplateTypeParmDecl>(p))
            checkTemplateArg(ttp->getDefaultArgument());
    // Manually traverse template function bodies since CRTP cannot
    // override TraverseFunctionDecl across module boundaries
    if (fd->getBody())
      walkStmt(fd->getBody());
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *me) {
    if (me) {
      checkDecl(me->getMemberDecl());
      // A member access on an alias specialization (`internal::is_callable_r<
      // int, C>::value`) references the alias only through the BASE's type;
      // check it (the desugared underlying template is traced here, and the
      // alias itself is added by the alias closure).
      if (auto *base = me->getBase()) checkType(base->getType());
    }
    return true;
  }

  bool VisitCXXDependentScopeMemberExpr(
      clang::CXXDependentScopeMemberExpr *me) {
    if (me) {
      // A dependent member access (`internal::is_callable_r<int, C>::value`)
      // has no resolved member decl; check its base type.
      if (!me->isImplicitAccess()) checkType(me->getBaseType());
    }
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *ce) {
    return checkDecl(ce ? ce->getConstructor() : nullptr);
  }

  bool VisitCXXTemporaryObjectExpr(clang::CXXTemporaryObjectExpr *toe) {
    return checkDecl(toe ? toe->getConstructor() : nullptr);
  }

  bool VisitInitListExpr(clang::InitListExpr *ile) {
    if (ile) checkType(ile->getType());
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *vd) {
    if (!vd) return true;
    if (!is_consumer_use(vd->getLocation())) return true;
    checkType(vd->getType());
    return true;
  }

  bool VisitFieldDecl(clang::FieldDecl *fd) {
    if (!fd) return true;
    if (!is_consumer_use(fd->getLocation())) return true;
    checkType(fd->getType());
    return true;
  }

  bool VisitTypedefDecl(clang::TypedefDecl *td) {
    if (!td) return true;
    if (!is_consumer_use(td->getLocation())) return true;
    checkType(td->getUnderlyingType());
    return true;
  }

  bool VisitTypeAliasDecl(clang::TypeAliasDecl *ta) {
    if (!ta) return true;
    if (!is_consumer_use(ta->getLocation())) return true;
    checkType(ta->getUnderlyingType());
    return true;
  }

  bool VisitFriendDecl(clang::FriendDecl *fd) {
    if (!fd) return true;
    if (auto *tsi = fd->getFriendType())
      checkType(tsi->getType());
    if (auto *d = fd->getFriendDecl())
      record_by_fqn(d);
    return true;
  }

  bool VisitFriendTemplateDecl(clang::FriendTemplateDecl *ftd) {
    if (!ftd) return true;
    if (auto *tsi = ftd->getFriendType())
      checkType(tsi->getType());
    if (auto *d = ftd->getFriendDecl())
      record_by_fqn(d);
    return true;
  }

  bool VisitClassTemplateSpecializationDecl(
      clang::ClassTemplateSpecializationDecl *d) {
    if (!d) return true;
    if (!is_consumer_use(d->getLocation())) return true;
    if (auto *as_written = d->getTemplateArgsAsWritten())
      for (auto &arg : as_written->arguments())
        checkTemplateArg(arg);
    return true;
  }

private:
  void checkTemplateArg(clang::TemplateArgumentLoc tal) {
    if (auto *tsi = tal.getTypeSourceInfo()) {
      checkType(tsi->getType());
      walkTypeLoc(tsi->getTypeLoc());
      return;
    }
    auto arg = tal.getArgument();
    switch (arg.getKind()) {
    case clang::TemplateArgument::Type:
      checkType(arg.getAsType());
      break;
    case clang::TemplateArgument::Expression:
      walkStmt(arg.getAsExpr());
      break;
    case clang::TemplateArgument::Pack:
      for (auto sub : arg.pack_elements())
        if (sub.getKind() == clang::TemplateArgument::Type)
          checkType(sub.getAsType());
      break;
    default:
      break;
    }
  }

  int depth_ = 0;

  void walkStmt(clang::Stmt *s) {
    if (!s || depth_ > 200) return;
    auto loc = sm.getExpansionLoc(s->getBeginLoc());
    if (loc.isValid() && !is_consumer_use(loc)) return;
    ScopeDepth g(depth_);
    if (auto *ce = llvm::dyn_cast<clang::CallExpr>(s))
      checkDecl(ce->getDirectCallee());
    if (auto *ule = llvm::dyn_cast<clang::UnresolvedLookupExpr>(s))
      for (auto *d : ule->decls())
        checkDecl(d);
    if (auto *cce = llvm::dyn_cast<clang::CXXConstructExpr>(s))
      checkDecl(cce->getConstructor());
    if (auto *ne = llvm::dyn_cast<clang::CXXNewExpr>(s))
      checkType(ne->getAllocatedType());
    if (auto *me = llvm::dyn_cast<clang::MemberExpr>(s)) {
      checkDecl(me->getMemberDecl());
      auto nns = me->getQualifier();
      if (nns.getKind() == clang::NestedNameSpecifier::Kind::Type)
        if (auto *t = nns.getAsType())
          checkType(clang::QualType(t, 0));
    }
    if (auto *ile = llvm::dyn_cast<clang::InitListExpr>(s))
      checkType(ile->getType());
    if (auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(s)) {
      checkDecl(dre->getDecl());
      auto nns = dre->getQualifier();
      if (nns.getKind() == clang::NestedNameSpecifier::Kind::Type)
        if (auto *t = nns.getAsType())
          checkType(clang::QualType(t, 0));
    }
    if (auto *dsdre = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(s)) {
      auto nns = dsdre->getQualifier();
      if (nns.getKind() == clang::NestedNameSpecifier::Kind::Type)
        if (auto *t = nns.getAsType())
          checkType(clang::QualType(t, 0));
    }
    if (auto *utte = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(s)) {
      if (utte->isArgumentType()) {
        if (auto *tsi = utte->getArgumentTypeInfo())
          checkType(tsi->getType());
      } else {
        walkStmt(utte->getArgumentExpr());
      }
    }
    for (auto *child : s->children())
      walkStmt(child);
  }

  void walkNNS(clang::NestedNameSpecifierLoc nns) {
    if (!nns) return;
    auto q = nns.getNestedNameSpecifier();
    if (q.getKind() == clang::NestedNameSpecifier::Kind::Type)
      if (auto tl = nns.getAsTypeLoc())
        walkTypeLoc(tl);
  }

  void walkTypeLoc(clang::TypeLoc tl) {
    if (tl.isNull()) return;
    if (auto tt = tl.getAs<clang::TypedefTypeLoc>()) {
      walkNNS(tt.getQualifierLoc());
      return;
    }
    if (auto tst = tl.getAs<clang::TemplateSpecializationTypeLoc>()) {
      for (unsigned i = 0; i < tst.getNumArgs(); ++i) {
        auto tal = tst.getArgLoc(i);
        auto arg = tal.getArgument();
        if (arg.getKind() == clang::TemplateArgument::Expression)
          walkStmt(tal.getSourceExpression());
        else if (auto *tsi = tal.getTypeSourceInfo())
          walkTypeLoc(tsi->getTypeLoc());
      }
      return;
    }
    if (auto next = tl.getNextTypeLoc())
      walkTypeLoc(next);
  }

  void checkType(clang::QualType qt) {
    if (qt.isNull() || depth_ > 200) return;
    ScopeDepth g(depth_);
    if (auto *td = qt->getAs<clang::TypedefType>()) {
      checkDecl(td->getDecl());
      checkType(td->desugar());
      return;
    }
    // Check alias/template names before canonicalization so that
    // `using X = ...;` and alias templates are traced by name.
    if (auto *tst = qt->getAs<clang::TemplateSpecializationType>())
      if (auto *tn = tst->getTemplateName().getAsTemplateDecl())
        checkDecl(tn->getTemplatedDecl());
    qt = qt.getCanonicalType();
    while (qt->isPointerType() || qt->isReferenceType())
      qt = qt->getPointeeType();
    if (qt.isConstQualified() || qt.isVolatileQualified())
      qt = qt.getUnqualifiedType();
    if (auto *rd = qt->getAsCXXRecordDecl()) {
      checkDecl(rd);
      return;
    }
    if (auto *ed = qt->getAsTagDecl()) {
      checkDecl(ed);
      return;
    }
    if (auto *tst = llvm::dyn_cast<clang::TemplateSpecializationType>(
            qt.getTypePtr())) {
      if (auto *td = tst->getTemplateName().getAsTemplateDecl())
        checkDecl(td->getTemplatedDecl());
      for (auto &arg : tst->template_arguments()) {
        if (arg.getKind() == clang::TemplateArgument::Type)
          checkType(arg.getAsType());
        else if (arg.getKind() == clang::TemplateArgument::Expression)
          walkStmt(arg.getAsExpr());
      }
      return;
    }
    if (auto *dnt = llvm::dyn_cast<clang::DependentNameType>(qt.getTypePtr())) {
      auto nns = dnt->getQualifier();
      if (nns.getKind() == clang::NestedNameSpecifier::Kind::Type)
        if (auto *t = nns.getAsType())
          checkType(clang::QualType(t, 0));
      return;
    }
  }

  bool checkDecl(clang::NamedDecl *nd) {
    if (!nd) return true;
    auto *cd = llvm::dyn_cast<clang::NamedDecl>(nd->getCanonicalDecl());
    if (!cd) return true;
    // Constructors/destructors are collected as their enclosing class
    if (auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(cd))
      cd = ctor->getParent();
    else if (auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(cd))
      cd = dtor->getParent();
    auto loc = sm.getExpansionLoc(cd->getLocation());
    if (!loc.isValid()) {
      if (auto *ctx = cd->getDeclContext())
        if (auto *parent = llvm::dyn_cast<clang::NamedDecl>(ctx))
          loc = sm.getExpansionLoc(parent->getCanonicalDecl()->getLocation());
      if (!loc.isValid()) return true;
    }
    auto fid = sm.getFileID(loc);
    if (fid == consumer_fid) return true;
    // Check the template arguments of a referenced specialization BEFORE the
    // producer lookup: the specialization itself (e.g. `std::is_base_of_v`)
    // lives in a system header and would otherwise return early, hiding the
    // nested internal types (`internal::disjunction<>`) that only appear as its
    // template arguments. Both the canonical (desugared) args and the
    // as-written args are checked, since alias templates desugar away in the
    // canonical argument list.
    auto check_spec_args = [&](auto *spec) {
      for (auto &arg : spec->getTemplateArgs().asArray())
        if (arg.getKind() == clang::TemplateArgument::Type)
          checkType(arg.getAsType());
      if (auto *written = spec->getTemplateArgsAsWritten())
        for (auto &argloc : written->arguments())
          if (auto *tsi = argloc.getTypeSourceInfo())
            checkType(tsi->getType());
    };
    if (auto *vt = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(cd))
      check_spec_args(vt);
    else if (auto *ct =
                 llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(cd))
      check_spec_args(ct);
    auto fe = sm.getFileEntryRefForID(fid);
    if (!fe) return true;
    auto producer_path = fe->getName().str();
    auto it = producer_internal.find(producer_path);
    if (it == producer_internal.end()) return true;
    // If the CONSUMER itself provides the definition (the library only
    // forward-declares the entity, e.g. a header fwd-declares
    // `SuccessChecker` and the test defines it), exporting the module's
    // forward declaration would collide with the consumer's own global-module
    // definition. A friend declaration (no definition) must still be traced.
    if (consumer_defined_in_main(cd, sm)) return true;
    auto fqn = cd->getQualifiedNameAsString();
    // Robust check: even when the redeclaration chain of the producer's
    // forward declaration does NOT contain the consumer's definition (a
    // separate same-name declaration chain can be produced by a contaminated
    // AST), the consumer may still define the entity. Look it up by qualified
    // name in the consumer's namespace and skip if any matching declaration
    // has a definition in the main file.
    if (consumer_defines_fqn(fqn)) return true;
    if (it->second.count(fqn)) reachable[producer_path].insert(fqn);
    return true;
  }

  // Returns true when the CONSUMER itself declares (and defines) an entity with
  // the given qualified name anywhere in the main file. Used as a robust
  // fallback to the getDefinition() checks above: a producer header may only
  // forward-declare an internal class while the consumer defines it, and in a
  // contaminated AST the two declarations can be separate chains that never
  // merge. The qualified-name lookup from the translation unit root finds the
  // definition regardless.
  bool consumer_defines_fqn(const std::string &fqn) const {
    if (!ctx_) return false;
    auto parts = split_on(fqn, ':');
    if (parts.empty()) return false;
    std::string name = parts.back();
    parts.pop_back();
    clang::DeclContext *dc = ctx_->getTranslationUnitDecl();
    for (auto &ns : parts) {
      clang::DeclContext *next = nullptr;
      for (auto *d : dc->lookup(
               clang::DeclarationName(&ctx_->Idents.get(ns)))) {
        if (auto *nd = llvm::dyn_cast<clang::NamespaceDecl>(d)) {
          next = nd;
          break;
        }
      }
      if (!next) return false;
      dc = next;
    }
    for (auto *d : dc->lookup(
             clang::DeclarationName(&ctx_->Idents.get(name)))) {
      if (auto *n = llvm::dyn_cast<clang::NamedDecl>(d))
        if (consumer_defined_in_main(n, sm)) return true;
    }
    return false;
  }

  // Records a friend-declaration target by FQN. A friend class template
  // declaration is a `friend_undeclared` decl located in the consumer, so
  // location-based producer detection fails; instead mark every producer that
  // declares the entity internal.
  void record_by_fqn(clang::NamedDecl *nd) {
    if (!nd) return;
    auto fqn = nd->getQualifiedNameAsString();
    if (fqn.empty()) return;
    // If the CONSUMER itself defines the friend target (the library only
    // forward-declares it, e.g. a header fwd-declares `ListenerAccessor`
    // and the test defines it), exporting it would collide with the consumer's
    // own global-module definition. A friend declaration without a definition
    // must still be traced.
    if (consumer_defined_in_main(nd, sm)) return;
    // Robust fallback (see checkDecl): the consumer may define the friend
    // target in a separate, un-merged declaration chain.
    if (consumer_defines_fqn(fqn)) return;
    for (auto &[producer, entities] : producer_internal)
      if (entities.count(fqn)) reachable[producer].insert(fqn);
  }

  const clang::SourceManager &sm;
  clang::FileID consumer_fid;
  clang::ASTContext *ctx_ = nullptr;
  const std::map<std::string, std::set<std::string>> &producer_internal;
  std::map<std::string, std::set<std::string>> &reachable;
  // The headers being rewritten (as given AND canonicalized, since a file
  // entry may be either). Null keeps the main-file-only rule.
  const std::set<std::string> *producer_files = nullptr;
};

export class ConsumerRefConsumer : public clang::ASTConsumer {
public:
  ConsumerRefConsumer(const clang::SourceManager &sm,
                      const std::map<std::string, std::set<std::string>>
                          &producer_internal,
                      std::map<std::string, std::set<std::string>> &reachable,
                      std::set<std::string> *consumer_defined,
                      const std::set<std::string> *producer_files = nullptr)
      : defined_collector(sm, consumer_defined),
        vis(sm, producer_internal, reachable, producer_files) {}

  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    if (defined_collector.enabled())
      defined_collector.TraverseDecl(ctx.getTranslationUnitDecl());
    vis.setContext(ctx);
    vis.TraverseDecl(ctx.getTranslationUnitDecl());
  }

private:
  InternalCollector defined_collector;
  ConsumerRefFinder vis;
};

// Extends the reachable set of a producer with internal aliases whose
// underlying type references an already-reachable internal entity. Consumer
// references to alias templates in dependent member access
// (`typename Alias<...>::type`) resolve in the AST to the underlying template,
// so the alias name itself is never traced; it must still be exported for
// sibling modules to use it.
export class AliasReachabilityCloser
    : public clang::RecursiveASTVisitor<AliasReachabilityCloser> {
public:
  AliasReachabilityCloser(const clang::SourceManager &sm,
                          const std::set<std::string> &producer_internal,
                          const std::set<std::string> &reachable,
                          std::set<std::string> &out)
      : sm(sm),
        main_fid(sm.getMainFileID()),
        producer_internal(producer_internal), reachable(reachable), out(out) {}

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  bool VisitTypeAliasDecl(clang::TypeAliasDecl *ta) {
    if (!ta || ta->isImplicit()) return true;
    auto loc = sm.getExpansionLoc(ta->getLocation());
    if (!loc.isValid() || sm.getFileID(loc) != main_fid) return true;
    auto fqn = ta->getQualifiedNameAsString();
    if (!producer_internal.count(fqn)) return true;
    if (references_reachable(ta->getUnderlyingType()))
      out.insert(fqn);
    return true;
  }

  bool VisitTypedefDecl(clang::TypedefDecl *td) {
    if (!td || td->isImplicit()) return true;
    auto loc = sm.getExpansionLoc(td->getLocation());
    if (!loc.isValid() || sm.getFileID(loc) != main_fid) return true;
    auto fqn = td->getQualifiedNameAsString();
    if (!producer_internal.count(fqn)) return true;
    if (references_reachable(td->getUnderlyingType()))
      out.insert(fqn);
    return true;
  }

  bool VisitTypeAliasTemplateDecl(clang::TypeAliasTemplateDecl *tat) {
    if (!tat || tat->isImplicit()) return true;
    auto loc = sm.getExpansionLoc(tat->getLocation());
    if (!loc.isValid() || sm.getFileID(loc) != main_fid) return true;
    auto fqn = tat->getQualifiedNameAsString();
    if (!producer_internal.count(fqn)) return true;
    auto *ta = tat->getTemplatedDecl();
    if (ta && references_reachable(ta->getUnderlyingType()))
      out.insert(fqn);
    return true;
  }

private:
  int depth_ = 0;

  bool references_reachable(clang::QualType qt) {
    if (qt.isNull() || depth_ > 100) return false;
    ScopeDepth g(depth_);
    if (auto *td = qt->getAs<clang::TypedefType>()) {
      if (reachable.count(td->getDecl()->getQualifiedNameAsString()))
        return true;
      return references_reachable(td->desugar());
    }
    if (auto *tst = qt->getAs<clang::TemplateSpecializationType>()) {
      if (auto *tn = tst->getTemplateName().getAsTemplateDecl())
        if (reachable.count(
                tn->getTemplatedDecl()->getQualifiedNameAsString()))
          return true;
      for (auto &arg : tst->template_arguments()) {
        if (arg.getKind() == clang::TemplateArgument::Type &&
            references_reachable(arg.getAsType()))
          return true;
        if (arg.getKind() == clang::TemplateArgument::Pack)
          for (auto sub : arg.pack_elements())
            if (sub.getKind() == clang::TemplateArgument::Type &&
                references_reachable(sub.getAsType()))
              return true;
      }
      return false;
    }
    qt = qt.getCanonicalType();
    while (qt->isPointerType() || qt->isReferenceType())
      qt = qt->getPointeeType();
    if (qt.isConstQualified() || qt.isVolatileQualified())
      qt = qt.getUnqualifiedType();
    if (auto *rd = qt->getAsCXXRecordDecl())
      return reachable.count(rd->getQualifiedNameAsString());
    if (auto *ed = qt->getAsTagDecl())
      return reachable.count(ed->getQualifiedNameAsString());
    if (auto *tst = llvm::dyn_cast<clang::TemplateSpecializationType>(
            qt.getTypePtr())) {
      if (auto *td = tst->getTemplateName().getAsTemplateDecl())
        if (reachable.count(
                td->getTemplatedDecl()->getQualifiedNameAsString()))
          return true;
      for (auto &arg : tst->template_arguments())
        if (arg.getKind() == clang::TemplateArgument::Type &&
            references_reachable(arg.getAsType()))
          return true;
      return false;
    }
    if (auto *dnt = llvm::dyn_cast<clang::DependentNameType>(qt.getTypePtr())) {
      auto nns = dnt->getQualifier();
      if (nns.getKind() == clang::NestedNameSpecifier::Kind::Type)
        if (auto *t = nns.getAsType())
          return references_reachable(clang::QualType(t, 0));
      return false;
    }
    return false;
  }

  const clang::SourceManager &sm;
  clang::FileID main_fid;
  const std::set<std::string> &producer_internal;
  const std::set<std::string> &reachable;
  std::set<std::string> &out;
};

// ── System-header usage tracing ─────────────────────────────────────

// A system header no consumer may include directly: an implementation detail
// that exists only to be pulled in by the public header owning the declaration.
// Covers the C library's arch/detail split (glibc's and musl's `bits/`) and the
// per-declaration fragments compilers ship (`__stddef_size_t.h` and friends).
bool is_private_system_header(llvm::StringRef path) {
  std::filesystem::path p(path.str());
  if (p.filename().string().starts_with("__")) return true;
  return std::ranges::any_of(p, [](const std::filesystem::path &part) {
    return part == "bits";
  });
}

// The name the consumer would write in an `#include <...>` to reach `fe`,
// relative to whichever search directory the header was found under — so a
// header in a subdirectory keeps it (`sys/types.h`, not `types.h`, which names
// nothing). Falls back to the bare filename when no search directory covers it.
// How a consumer should spell an include of `fe`, as an ANGLED include: the
// re-added includes are emitted `#include <...>`, so the spelling has to
// resolve against the search path.
//
// `relative_to_main` decides whether clang may answer with a path relative to
// the including file. For a system header that is harmless — it never happens.
// For a header beside the consumer it produces a spelling that only works in
// quotes, and emitted in angle brackets it resolves to nothing:
//
//   test/production.cc:35:10: fatal error: production.h: No such file
//     35 | #include <production.h>
//
// Denied the shortcut, clang answers with the search-dir-relative
// `test/production.h`, which is what an angled include needs.
std::string include_spelling_for(clang::FileEntryRef fe,
                                 const clang::SourceManager &sm,
                                 clang::FileID main_fid,
                                 const clang::HeaderSearch *hs,
                                 bool relative_to_main = true) {
  std::string fallback =
      std::filesystem::path(fe.getName().str()).filename().string();
  if (!hs) return fallback;
  std::string main_path;
  if (relative_to_main)
    if (auto mfe = sm.getFileEntryRefForID(main_fid))
      main_path = mfe->getName().str();
  auto spelled = hs->suggestPathToFileForDiagnostics(fe, main_path);
  if (spelled.empty() || spelled.contains("..") ||
      std::filesystem::path(spelled).is_absolute())
    return relative_to_main ? fallback : std::string();
  return spelled;
}

// Shared: derive the include name (e.g. "errno.h", "sys/types.h") a consumer
// should write to get a macro/symbol defined at `loc`. Walks the include chain
// from the defining file toward the consumer's main file and takes the
// INNERMOST public system header on it — the one that owns the declaration.
//
// Not the outermost: that is merely whichever C header the library happened to
// pull in first, and it provides the declaration only transitively, on the
// libc the conversion ran against. Reporting `stdlib.h` for `ssize_t` because
// glibc's `stdlib.h` reaches `sys/types.h` produces a tree that builds on
// glibc and fails on musl, whose `stdlib.h` does not.
//
// C++ stdlib headers (under `/c++/`) are skipped entirely — `import std.compat`
// provides those — as are private headers, whose public parent is taken instead.
std::string system_include_name_for(clang::SourceLocation loc,
                                    const clang::SourceManager &sm,
                                    clang::FileID main_fid,
                                    const clang::HeaderSearch *hs = nullptr) {
  auto fid = sm.getFileID(loc);
  while (fid.isValid() && fid != main_fid) {
    if (auto fe = sm.getFileEntryRefForID(fid)) {
      auto path = fe->getName();
      if (sm.isInSystemHeader(sm.getLocForStartOfFile(fid)) &&
          !path.contains("/c++/") && !is_private_system_header(path))
        return include_spelling_for(*fe, sm, main_fid, hs);
    }
    auto inc = sm.getIncludeLoc(fid);
    if (!inc.isValid()) break;
    fid = sm.getFileID(inc);
  }
  return {};
}

// Derive the include a consumer must write to get an entity defined at `loc`
// when that entity comes from a header OUTSIDE the library being converted.
//
// Converting a library does not convert what it includes. Its module units
// keep those includes in the global module fragment, where the declarations
// attach to the global module — reachable while compiling the unit, invisible
// to anyone importing it. A macro does not cross a module boundary at all. So
// a consumer that used to get these through the library header now gets
// nothing, and has to include the header itself:
//
//   error: missing '#include "boost/assert/source_location.hpp"';
//          'source_location' must be declared before it is used
//
// The defining file itself, not anything further out on the include chain: an
// outer header provides the declaration only by having happened to include
// this one.
std::string outside_include_name_for(
    clang::SourceLocation loc, const clang::SourceManager &sm,
    clang::FileID main_fid, const clang::HeaderSearch *hs,
    const std::set<std::string> &library_files) {
  auto fid = sm.getFileID(loc);
  if (!fid.isValid() || fid == main_fid) return {};
  auto fe = sm.getFileEntryRefForID(fid);
  if (!fe) return {};
  // A header of the library itself becomes a module. Importing it is what
  // supplies these declarations, and re-adding the include beside that import
  // declares everything in it twice.
  std::error_code ec;
  auto canon = std::filesystem::weakly_canonical(
      std::filesystem::path(fe->getName().str()), ec);
  if (library_files.count(ec ? fe->getName().str() : canon.string())) return {};
  // The C++ standard library belongs to `import std`, and the C headers it
  // cannot cover are traced as system includes already.
  if (fe->getName().contains("/c++/") ||
      sm.isInSystemHeader(sm.getLocForStartOfFile(fid)))
    return {};
  return include_spelling_for(*fe, sm, main_fid, hs,
                              /*relative_to_main=*/false);
}

// PP callbacks: records the system header of every macro EXPANDED in the
// consumer's main file. Macros defined in the consumer itself or in library
// headers produce no include (library macro headers are handled by the include
// map); a macro defined in a C/POSIX system header (errno, assert, INT_MAX,
// NULL, stderr, PTHREAD_*, ...) records the header the consumer must include,
// since `import std.compat` cannot provide the C library's macros.
export class SystemHeaderUsageTracer : public clang::PPCallbacks {
public:
  using Resolver = std::function<std::string(clang::SourceLocation)>;

  SystemHeaderUsageTracer(clang::CompilerInstance &ci, std::set<std::string> &out,
                          Resolver resolver = {})
      : sm(ci.getSourceManager()), main_fid(ci.getSourceManager().getMainFileID()),
        hs(&ci.getPreprocessor().getHeaderSearchInfo()), out(out),
        resolver(std::move(resolver)) {}

  void MacroExpands(const clang::Token &MacroNameTok,
                    const clang::MacroDefinition &MD,
                    clang::SourceRange Range,
                    const clang::MacroArgs *Args) override {
    // Only macros the consumer's own code (or a library macro it invokes)
    // expands at a main-file call site matter.
    if (!sm.isInMainFile(sm.getExpansionLoc(Range.getBegin()))) return;
    auto *mi = MD.getMacroInfo();
    if (!mi || mi->isBuiltinMacro() || mi->isUsedForHeaderGuard()) return;
    auto def_loc = mi->getDefinitionLoc();
    if (!def_loc.isValid() || sm.isInMainFile(def_loc)) return;
    auto name = resolver ? resolver(def_loc)
                         : system_include_name_for(def_loc, sm, main_fid, hs);
    if (!name.empty()) out.insert(std::move(name));
  }

private:
  const clang::SourceManager &sm;
  clang::FileID main_fid;
  const clang::HeaderSearch *hs;
  std::set<std::string> &out;
  Resolver resolver;
};

// AST visitor: records the system header of every declaration (function, type,
// typedef, variable) referenced in the consumer's main file. Covers POSIX
// declarations/types (pthread_mutex_lock, ssize_t, pthread_mutex_t) and C
// library functions (printf, fflush) that the C++ stdlib module cannot export.
export class SystemHeaderRefFinder
    : public clang::RecursiveASTVisitor<SystemHeaderRefFinder> {
public:
  using Resolver = std::function<std::string(clang::SourceLocation)>;

  SystemHeaderRefFinder(const clang::SourceManager &sm,
                        const clang::HeaderSearch *hs,
                        std::set<std::string> &out, Resolver resolver = {})
      : sm(sm), main_fid(sm.getMainFileID()), hs(hs), out(out),
        resolver(std::move(resolver)) {}

  bool shouldVisitImplicitCode() const { return true; }
  bool shouldVisitTemplateInstantiations() const { return true; }

  bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
    if (e && from_main(e->getBeginLoc())) record(e->getDecl());
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *e) {
    if (e && from_main(e->getBeginLoc())) record(e->getDirectCallee());
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *e) {
    if (e && from_main(e->getBeginLoc())) record(e->getConstructor());
    return true;
  }

  bool VisitCXXTemporaryObjectExpr(clang::CXXTemporaryObjectExpr *e) {
    if (e && from_main(e->getBeginLoc())) record(e->getConstructor());
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *e) {
    if (e && from_main(e->getBeginLoc())) record(e->getMemberDecl());
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *d) {
    if (d && from_main(d->getBeginLoc())) record_type(d->getType());
    return true;
  }

  bool VisitFieldDecl(clang::FieldDecl *d) {
    if (d && from_main(d->getBeginLoc())) record_type(d->getType());
    return true;
  }

  bool VisitTypedefDecl(clang::TypedefDecl *d) {
    if (d && from_main(d->getBeginLoc())) record_type(d->getUnderlyingType());
    return true;
  }

  bool VisitTypeAliasDecl(clang::TypeAliasDecl *d) {
    if (d && from_main(d->getBeginLoc())) record_type(d->getUnderlyingType());
    return true;
  }

  bool VisitTypeLoc(clang::TypeLoc tl) {
    if (tl && from_main(tl.getBeginLoc())) record_type(tl.getType());
    return true;
  }

  bool VisitFriendDecl(clang::FriendDecl *d) {
    if (!d) return true;
    if (auto *tsi = d->getFriendType()) record_type(tsi->getType());
    if (auto *nd = d->getFriendDecl()) record(nd);
    return true;
  }

private:
  bool from_main(clang::SourceLocation loc) const {
    auto el = sm.getExpansionLoc(loc);
    return el.isValid() && sm.isInMainFile(el);
  }

  void record_type(clang::QualType qt) {
    if (qt.isNull() || depth_ > 100) return;
    ScopeDepth g(depth_);
    if (auto *td = qt->getAs<clang::TypedefType>()) {
      record(td->getDecl());
      record_type(td->desugar());
      return;
    }
    if (auto *tst = qt->getAs<clang::TemplateSpecializationType>()) {
      if (auto *td = tst->getTemplateName().getAsTemplateDecl())
        record(td->getTemplatedDecl());
      for (auto &arg : tst->template_arguments())
        if (arg.getKind() == clang::TemplateArgument::Type)
          record_type(arg.getAsType());
      return;
    }
    qt = qt.getCanonicalType();
    while (qt->isPointerType() || qt->isReferenceType())
      qt = qt->getPointeeType();
    if (auto *rd = qt->getAsCXXRecordDecl()) {
      record(rd);
      return;
    }
    if (auto *ed = qt->getAsTagDecl()) {
      record(ed);
      return;
    }
    if (auto *tst = llvm::dyn_cast<clang::TemplateSpecializationType>(
            qt.getTypePtr())) {
      if (auto *td = tst->getTemplateName().getAsTemplateDecl())
        record(td->getTemplatedDecl());
      for (auto &arg : tst->template_arguments())
        if (arg.getKind() == clang::TemplateArgument::Type)
          record_type(arg.getAsType());
    }
  }

  void record(clang::NamedDecl *nd) {
    if (!nd) return;
    auto *cd = llvm::dyn_cast<clang::NamedDecl>(nd->getCanonicalDecl());
    if (!cd) return;
    if (auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(cd))
      cd = ctor->getParent();
    else if (auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(cd))
      cd = dtor->getParent();
    auto loc = sm.getExpansionLoc(cd->getLocation());
    if (!loc.isValid() || sm.isInMainFile(loc)) return;
    auto name = resolver ? resolver(loc)
                         : system_include_name_for(loc, sm, main_fid, hs);
    if (!name.empty()) out.insert(std::move(name));
  }

  int depth_ = 0;
  const clang::SourceManager &sm;
  clang::FileID main_fid;
  const clang::HeaderSearch *hs;
  std::set<std::string> &out;
  Resolver resolver;
};

// Frontend-action factory that installs the PP macro tracer and runs the AST
// visitor, both recording into the same per-consumer include set.
// PP callbacks: records every header a LIBRARY header includes that the
// library does not own.
//
// Not for the library's sake — its own units include these in their global
// module fragment and compile fine. For its consumers'. A module's global
// module fragment is pruned to what is decl-reachable from its exported
// declarations, so an outside type named in the interface survives while an
// operator found only by ADL, at a template instantiation in the consumer's
// own translation unit, does not:
//
//   error: invalid operands to binary expression
//   note: candidate function not viable: operator==(monostate, monostate)
//
// The header was plainly visible and the one operator that mattered was gone.
// Before the conversion the consumer got all of this transitively through the
// library header; an import gives back only part, so the rest is written out.
export class LibraryOutsideIncludeTracer : public clang::PPCallbacks {
public:
  LibraryOutsideIncludeTracer(clang::CompilerInstance &ci,
                              const std::set<std::string> &library_files,
                              const std::set<std::string> &provided,
                              const std::set<std::string> &transparent_macros,
                              std::set<std::string> &out)
      : sm(ci.getSourceManager()), lo(ci.getLangOpts()),
        library_files(library_files), provided(provided),
        transparent_macros(transparent_macros), out(out) {}

  // Only what the library includes UNCONDITIONALLY is the consumer's to
  // include too. A header reached only on another platform, or behind a
  // feature test, is handed to a build that cannot compile it:
  //
  //   boost/winapi/basic_types.hpp:38:3: error: "Win32 functions not available"
  //
  // The conversion's own `<PREFIX>_USE_MODULES` guard is the exception: every
  // generated header wraps its includes in it, and it says nothing about
  // whether the include was conditional before the conversion.
  void If(clang::SourceLocation, clang::SourceRange range,
          ConditionValueKind) override {
    push(condition_text(range));
  }
  void Ifdef(clang::SourceLocation, const clang::Token &tok,
             const clang::MacroDefinition &) override {
    push(tok.getIdentifierInfo() ? tok.getIdentifierInfo()->getName().str() : "");
  }
  void Ifndef(clang::SourceLocation, const clang::Token &tok,
              const clang::MacroDefinition &) override {
    auto name = tok.getIdentifierInfo()
                    ? tok.getIdentifierInfo()->getName()
                    : llvm::StringRef();
    // A header's own include guard is not a condition on what it includes: it
    // wraps the whole file and is true exactly once. Counting it makes EVERY
    // include in EVERY guarded header conditional, which is every header there
    // is, and the trace then reports nothing at all.
    if (looks_like_guard_name(name))
      conds.push_back(false);
    else
      push(name.str());
  }
  void Elif(clang::SourceLocation, clang::SourceRange range, ConditionValueKind,
            clang::SourceLocation) override {
    if (!conds.empty()) conds.back() = !is_transparent(condition_text(range));
  }
  void Endif(clang::SourceLocation, clang::SourceLocation) override {
    if (!conds.empty()) conds.pop_back();
  }

  void InclusionDirective(clang::SourceLocation hash_loc,
                          const clang::Token &, llvm::StringRef file_name,
                          bool is_angled, clang::CharSourceRange,
                          clang::OptionalFileEntryRef file, llvm::StringRef,
                          llvm::StringRef, const clang::Module *, bool,
                          clang::SrcMgr::CharacteristicKind file_type) override {
    if (!file) return;
    // Angled only. A quoted include is resolved relative to the header that
    // wrote it, and the consumer sits somewhere else entirely, so the same
    // spelling handed on resolves to nothing:
    //
    //   fatal error: production.h: No such file or directory
    if (!is_angled) return;
    // Only what the LIBRARY reaches for. An outside header's own includes are
    // that header's business and arrive with it.
    if (!in_library(hash_loc)) return;
    if (conditional()) return;
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(
        std::filesystem::path(file->getName().str()), ec);
    auto path = ec ? file->getName().str() : canon.string();
    if (library_files.count(path)) return;          // its own; becomes a module
    if (provided.count(file_name.str())) return;    // some module provides it
    // The standard library is `import std`'s business, and the C headers it
    // cannot cover are traced from the consumer's own usage.
    if (file->getName().contains("/c++/")) return;
    if (sm.isInSystemHeader(hash_loc)) return;
    // Where the header was FOUND, which the callback already knows: anything
    // reached through a system search path is not the library's to hand on.
    if (file_type != clang::SrcMgr::C_User) return;
    out.insert(file_name.str());
  }

private:
  void push(const std::string &cond) { conds.push_back(!is_transparent(cond)); }

  // A guard the conversion itself wrote: the `<PREFIX>_USE_MODULES` wrapper
  // every generated header carries, and the `_IMPORT_MODULES` guard of each
  // module that provides headers. Neither says the include was conditional
  // before the conversion, and for the second the caller emits both branches
  // anyway.
  bool is_transparent(const std::string &cond) const {
    for (const auto &m : transparent_macros)
      if (!m.empty() && cond.contains(m)) return true;
    return false;
  }

  bool conditional() const {
    for (bool b : conds)
      if (b) return true;
    return false;
  }

  std::string condition_text(clang::SourceRange range) const {
    auto text = clang::Lexer::getSourceText(
        clang::CharSourceRange::getTokenRange(range), sm, lo);
    return text.str();
  }

  bool in_library(clang::SourceLocation loc) const {
    auto fid = sm.getFileID(sm.getExpansionLoc(loc));
    auto fe = sm.getFileEntryRefForID(fid);
    if (!fe) return false;
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(
        std::filesystem::path(fe->getName().str()), ec);
    return library_files.count(ec ? fe->getName().str() : canon.string()) != 0;
  }

  const clang::SourceManager &sm;
  const clang::LangOptions &lo;
  const std::set<std::string> &library_files;
  const std::set<std::string> &provided;
  const std::set<std::string> &transparent_macros;
  std::set<std::string> &out;
  // One entry per open conditional: true when it is a real condition, false
  // when it is only the conversion's own guard.
  std::vector<bool> conds;
};

export class LibraryOutsideIncludeFactory
    : public clang::tooling::FrontendActionFactory {
public:
  LibraryOutsideIncludeFactory(std::set<std::string> &out,
                               const std::set<std::string> &library_files,
                               const std::set<std::string> &provided,
                               const std::set<std::string> &transparent_macros)
      : out(out), library_files(library_files), provided(provided),
        transparent_macros(transparent_macros) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<Action>(out, library_files, provided,
                                    transparent_macros);
  }

private:
  class Action : public clang::PreprocessOnlyAction {
  public:
    Action(std::set<std::string> &out, const std::set<std::string> &library_files,
           const std::set<std::string> &provided,
           const std::set<std::string> &transparent_macros)
        : out(out), library_files(library_files), provided(provided),
          transparent_macros(transparent_macros) {}

    bool BeginSourceFileAction(clang::CompilerInstance &ci) override {
      ci.getPreprocessor().addPPCallbacks(
          std::make_unique<LibraryOutsideIncludeTracer>(
              ci, library_files, provided, transparent_macros, out));
      return true;
    }

  private:
    std::set<std::string> &out;
    const std::set<std::string> &library_files;
    const std::set<std::string> &provided;
    const std::set<std::string> &transparent_macros;
  };

  std::set<std::string> &out;
  const std::set<std::string> &library_files;
  const std::set<std::string> &provided;
  const std::set<std::string> &transparent_macros;
};

// The same pair of passes, resolving to headers OUTSIDE the library rather
// than to system headers: what the consumer used to reach through the library
// header and can no longer reach through the import that replaced it.
export class OutsideHeaderUsageFactory
    : public clang::tooling::FrontendActionFactory {
public:
  OutsideHeaderUsageFactory(std::set<std::string> &out,
                            const std::set<std::string> &library_files)
      : out(out), library_files(library_files) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<Action>(out, library_files);
  }

private:
  class Action : public clang::ASTFrontendAction {
  public:
    Action(std::set<std::string> &out, const std::set<std::string> &library_files)
        : out(out), library_files(library_files) {}

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance &ci, llvm::StringRef) override {
      auto &sm = ci.getSourceManager();
      auto *hs = &ci.getPreprocessor().getHeaderSearchInfo();
      auto main_fid = sm.getMainFileID();
      auto &lib = library_files;
      // By value into both passes. make_traverse_consumer keeps an lvalue
      // argument BY REFERENCE — its referents are expected to outlive the
      // factory — and this one would die at the end of this function.
      SystemHeaderRefFinder::Resolver resolve =
          [&sm, hs, main_fid, &lib](clang::SourceLocation loc) {
            return outside_include_name_for(loc, sm, main_fid, hs, lib);
          };
      ci.getPreprocessor().addPPCallbacks(
          std::make_unique<SystemHeaderUsageTracer>(ci, out, resolve));
      return make_traverse_consumer<SystemHeaderRefFinder>(hs, out,
                                                           std::move(resolve));
    }

  private:
    std::set<std::string> &out;
    const std::set<std::string> &library_files;
  };

  std::set<std::string> &out;
  const std::set<std::string> &library_files;
};

export class SystemHeaderUsageFactory
    : public clang::tooling::FrontendActionFactory {
public:
  explicit SystemHeaderUsageFactory(std::set<std::string> &out) : out(out) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<Action>(out);
  }

private:
  class Action : public clang::ASTFrontendAction {
  public:
    explicit Action(std::set<std::string> &out) : out(out) {}

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance &ci, llvm::StringRef) override {
      ci.getPreprocessor().addPPCallbacks(
          std::make_unique<SystemHeaderUsageTracer>(ci, out));
      return make_traverse_consumer<SystemHeaderRefFinder>(
          &ci.getPreprocessor().getHeaderSearchInfo(), out);
    }

  private:
    std::set<std::string> &out;
  };

  std::set<std::string> &out;
};
