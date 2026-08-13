module;

export module modulizer.astutil;
import libtooling;
import std;

// Generic frontend-action factory: replaces the repetitive per-visitor
// Consumer/Action/Factory triplets. The caller supplies a maker that builds
// an ASTConsumer from the CompilerInstance (capturing any state by reference,
// which must outlive the tool.run() call).
export class VisitorFrontendActionFactory
    : public clang::tooling::FrontendActionFactory {
public:
  using ConsumerMaker =
      std::function<std::unique_ptr<clang::ASTConsumer>(clang::CompilerInstance &)>;

  explicit VisitorFrontendActionFactory(ConsumerMaker make)
      : make_(std::move(make)) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<Action>(make_);
  }

private:
  class Action : public clang::ASTFrontendAction {
  public:
    explicit Action(ConsumerMaker make) : make_(std::move(make)) {}

    std::unique_ptr<clang::ASTConsumer>
    CreateASTConsumer(clang::CompilerInstance &ci, llvm::StringRef) override {
      return make_(ci);
    }

  private:
    ConsumerMaker make_;
  };

  ConsumerMaker make_;
};

// Shared macro-eligibility check used by the entity/macro collectors and the
// header rewriter. Returns the MacroInfo when a #define is worth collecting
// (defined in the main file, not a system/builtin macro, not a header guard),
// otherwise nullptr.
export const clang::MacroInfo *collectible_macro(
    const clang::MacroDirective *md, const clang::SourceManager &sm) {
  if (!md) return nullptr;
  auto *mi = md->getMacroInfo();
  if (!mi || mi->isBuiltinMacro() || mi->isUsedForHeaderGuard()) return nullptr;
  auto loc = mi->getDefinitionLoc();
  if (!loc.isValid() || !sm.isInMainFile(loc) || sm.isInSystemHeader(loc))
    return nullptr;
  return mi;
}

// True when a declaration is at namespace scope (its enclosing chain reaches a
// namespace or the translation unit without first passing through a class,
// function, template, enum, or alias). Entity extraction and the header
// rewriter both rely on this to reject members, locals, and template
// parameters, which cannot be re-exported as namespace-scope names.
export bool is_namespace_scope(const clang::NamedDecl *d) {
  if (!d) return false;
  for (auto *dc = d->getDeclContext(); dc; dc = dc->getParent()) {
    if (llvm::isa<clang::NamespaceDecl>(dc)) return true;
    if (llvm::isa<clang::TranslationUnitDecl>(dc)) return true;
    if (llvm::isa<clang::FunctionDecl>(dc) ||
        llvm::isa<clang::CXXRecordDecl>(dc) ||
        llvm::isa<clang::ClassTemplateDecl>(dc) ||
        llvm::isa<clang::FunctionTemplateDecl>(dc) ||
        llvm::isa<clang::EnumDecl>(dc) ||
        llvm::isa<clang::TypeAliasTemplateDecl>(dc) ||
        llvm::isa<clang::TypeAliasDecl>(dc))
      return false;
  }
  return false;
}

// CRTP base for RecursiveASTVisitor subclasses that walk namespace scopes.
// Maintains the current namespace path (`ns_path_`) and, via the derived
// `is_internal_ns` hook, the internal-namespace depth (`internal_depth_`).
// Anonymous and inline namespaces are skipped from the path but still
// traversed. The hook is called after the namespace name is pushed (so it can
// see the full path) and before the depth is incremented (so it sees the
// parent depth), matching all three historical visitors.
export template <typename Derived>
class NsPathVisitor : public clang::RecursiveASTVisitor<Derived> {
public:
  // Deducing-this: the hook is found on the concrete derived visitor type
  // without a `static_cast<Derived *>(this)`.
  template <typename Self>
  bool TraverseNamespaceDecl(this Self &self, clang::NamespaceDecl *nd) {
    if (!nd) return true;
    auto &base = static_cast<clang::RecursiveASTVisitor<Derived> &>(self);
    if (nd->isAnonymousNamespace() || nd->isInline())
      return base.TraverseNamespaceDecl(nd);
    auto name = nd->getNameAsString();
    self.ns_path_.push_back(name);
    bool in_internal = self.is_internal_ns(name);
    if (in_internal) ++self.internal_depth_;
    bool result = base.TraverseNamespaceDecl(nd);
    if (in_internal) --self.internal_depth_;
    self.ns_path_.pop_back();
    return result;
  }

protected:
  std::vector<std::string> ns_path_;
  int internal_depth_ = 0;
};

// Shared declaration-reference scanner: traverses a declaration and records
// every library entity it names via the parent's `record(NamedDecl*)` hook.
// Used by the analyzer's and header rewriter's cross-module definition scans —
// alias (templates) copy their whole RHS, so unqualified same-namespace
// references (`using X = Impl<T>;`) must resolve to the cross-module entity.
export template <typename Parent>
class DeclRefScanner
    : public clang::RecursiveASTVisitor<DeclRefScanner<Parent>> {
public:
  explicit DeclRefScanner(Parent &parent) : parent_(parent) {}

  bool shouldVisitTemplateInstantiations() const { return false; }

  bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
    if (e) parent_.record(e->getDecl());
    return true;
  }

  bool VisitTypeLoc(clang::TypeLoc tl) {
    if (!tl) return true;
    auto *t = tl.getTypePtr();
    if (!t) return true;
    if (auto *td = t->getAsTagDecl()) {
      parent_.record(td);
      return true;
    }
    if (auto *tst = llvm::dyn_cast<clang::TemplateSpecializationType>(t)) {
      if (auto *td = tst->getTemplateName().getAsTemplateDecl())
        parent_.record(td);
    }
    return true;
  }

private:
  Parent &parent_;
};

// Shared statement-reference scanner: records the callees/declrefs of a
// trailing-return `decltype` expression (`auto f(...) -> decltype(f(...))`),
// which may name cross-module helpers (e.g. `apply_impl`). Same hook contract
// as DeclRefScanner.
export template <typename Parent>
class CallRefScanner
    : public clang::RecursiveASTVisitor<CallRefScanner<Parent>> {
public:
  explicit CallRefScanner(Parent &parent) : parent_(parent) {}

  bool shouldVisitTemplateInstantiations() const { return false; }

  bool VisitCallExpr(clang::CallExpr *e) {
    if (!e) return true;
    if (auto *fd = e->getDirectCallee()) {
      parent_.record(fd);
      return true;
    }
    if (auto *e2 = e->getCallee()) {
      if (auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(e2)) {
        parent_.record(dre->getDecl());
      } else if (auto *ule =
                     llvm::dyn_cast<clang::UnresolvedLookupExpr>(e2)) {
        for (auto *d : ule->decls())
          parent_.record(llvm::dyn_cast_or_null<clang::NamedDecl>(d));
      }
    }
    return true;
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
    if (e) parent_.record(e->getDecl());
    return true;
  }

private:
  Parent &parent_;
};

// Generic ASTConsumer that runs a single RecursiveASTVisitor over the
// translation unit: replaces the repetitive per-visitor
// `class XxxConsumer { HandleTranslationUnit { XxxVisitor v(...); v.TraverseDecl(...); } }`
// wrapper classes. `make_traverse_consumer<Visitor>(args...)` constructs
// `Visitor(sm, args...)` (where `sm` is the TU's SourceManager) and traverses.
export class TraverseConsumer : public clang::ASTConsumer {
public:
  using Handler = std::function<void(clang::ASTContext &)>;

  explicit TraverseConsumer(Handler handler) : handler_(std::move(handler)) {}

  void HandleTranslationUnit(clang::ASTContext &ctx) override { handler_(ctx); }

private:
  Handler handler_;
};

export template <typename Visitor, typename... Args>
std::unique_ptr<TraverseConsumer> make_traverse_consumer(Args &&...args) {
  // Store the constructor arguments decayed: references stay references (their
  // referents outlive the factory), value/prvalue arguments (e.g. a `&out`
  // pointer) are copied so the handler never dangles.
  auto stored = std::tuple<Args...>(std::forward<Args>(args)...);
  return std::make_unique<TraverseConsumer>(
      [stored = std::move(stored)](clang::ASTContext &ctx) mutable {
        std::apply(
            [&](auto &...a) {
              Visitor visitor(ctx.getSourceManager(), a...);
              visitor.TraverseDecl(ctx.getTranslationUnitDecl());
            },
            stored);
      });
}
