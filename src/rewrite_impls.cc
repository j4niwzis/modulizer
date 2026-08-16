module;

export module modulizer.rewrite_impls;
import modulizer.rewrite_util;
import libtooling;
import std;

// Collects definitions in an implementation unit that must be `extern "C++"`:
// out-of-line member definitions of classes forward-declared by another header
// (whose interface definition is therefore `extern "C++"`) and definitions of
// free functions/classes in fwd_declared_fqns. Without the prefix these would
// be distinct module entities that conflict with the global-module entities
// from the interface.

export class ExternCxxDefCollector
    : public clang::RecursiveASTVisitor<ExternCxxDefCollector> {
public:
  // `marker` is how `extern "C++"` is spelled here. A tree generated with the
  // whole-purview wrapping defers the choice to build time and passes the macro
  // that expands to nothing when the purview is wrapped — the definitions are
  // then already in the global module — and to `extern "C++"` when it is not.
  ExternCxxDefCollector(const clang::SourceManager &sm,
                        const std::set<std::string> &fwd_declared_fqns,
                        const std::set<std::string> &extern_classes,
                        std::vector<ModPoint> &mods,
                        std::string marker = "extern \"C++\" ",
                        std::string wrap_guard = {})
      : sm(sm), fwd_declared_fqns(fwd_declared_fqns),
        extern_classes(extern_classes), mods(mods),
        marker(std::move(marker)), wrap_guard(std::move(wrap_guard)) {}

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  bool VisitFunctionDecl(clang::FunctionDecl *fd) {
    if (!fd || fd->isImplicit() || !fd->isThisDeclarationADefinition())
      return true;
    auto loc = fd->getLocation();
    if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
    // A namespace-scope `static` function has internal linkage; extern "C++"
    // cannot be applied to it (and it is file-local, so it never needs to
    // merge across modules).
    if (fd->getStorageClass() == clang::SC_Static) return true;
    bool need = false;
    if (auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
      // An in-class member definition is already part of the class; the
      // extern "C++" prefix would be a syntax error here.
      if (llvm::isa<clang::CXXRecordDecl>(md->getLexicalDeclContext()))
        return true;
      if (auto *cls = llvm::dyn_cast<clang::CXXRecordDecl>(
              md->getDeclContext()))
        need = class_or_enclosing_cross_module(cls);
    } else {
      // `main` may not be a module entity: attaching it to the global module
      // (extern "C++") keeps it a valid program entry point from an
      // implementation unit. (getDeclName().isIdentifier() guards against
      // operator/constructor names, whose getName() would assert.)
      if (fd->getDeclName().isIdentifier() && fd->getName() == "main")
        need = true;
      else need = is_cross_module(fd);
    }
    if (need) {
      auto begin = sm.getExpansionLoc(fd->getBeginLoc());
      if (begin.isValid()) {
        unsigned off = sm.getFileOffset(begin);
        // Back up over a leading attribute macro (`LIB_API_ int main`)
        // so the marker precedes it (`extern "C++" LIB_API_ int main`).
        auto fid = sm.getFileID(begin);
        auto src = sm.getBufferData(fid);
        if (off > 0) {
          auto p = off - 1;
          while (p > 0 && (src[p] == ' ' || src[p] == '\t')) --p;
          if (p >= 1 && src[p] == ']' && src[p - 1] == ']') {
            while (p > 0 && (src[p] != '[' || src[p - 1] != '[')) --p;
            if (p > 0) off = p - 1;
          } else if (fd->hasAttrs() && p > 0 && is_ident_char(src[p])) {
            while (p > 0 && is_ident_char(src[p - 1])) --p;
            off = p;
          }
        }
        mods.push_back({off, 0, marker});
      }
    }
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *vd) {
    if (!vd || vd->isImplicit() || !vd->isThisDeclarationADefinition())
      return true;
    auto loc = vd->getLocation();
    if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
    if (!vd->isFileVarDecl()) return true;
    bool need = false;
    if (llvm::isa<clang::CXXRecordDecl>(vd->getDeclContext())) {
      // Out-of-line static member-variable definition of a cross-module class.
      if (auto *cls = llvm::dyn_cast<clang::CXXRecordDecl>(
              vd->getDeclContext()))
        need = class_or_enclosing_cross_module(cls);
    } else {
      need = is_cross_module(vd);
    }
    if (need) {
      auto begin = vd->getBeginLoc();
      auto expansion = sm.getExpansionLoc(begin);
      if (!expansion.isValid()) return true;
      auto fid = sm.getFileID(expansion);
      auto buf = sm.getBufferData(fid);
      unsigned begin_off = sm.getFileOffset(expansion);
      // A macro-expanded definition (e.g. `LIB_DEFINE_BOOL_(...)` whose body
      // is `namespace lib { bool FLAG_x = ...; }`) would only get a
      // single-statement `extern "C++"` prefix, leaving the definition inside
      // the macro's namespace as a module entity that conflicts with the
      // interface's global-module declaration. Wrap the whole invocation in an
      // `extern "C++" { ... }` block instead so the namespace (and the
      // variable) join the global module.
      bool macro_expanded = begin.isMacroID();
      if (macro_expanded) {
        // Find the end of the macro invocation: scan to the matching ')' or
        // the end of the current statement.
        auto end = sm.getExpansionLoc(vd->getEndLoc());
        unsigned end_off = sm.getFileOffset(end);
        while (end_off < buf.size() && buf[end_off] != ';') ++end_off;
        if (end_off < buf.size()) ++end_off;
        // A braced block cannot be spelled with the marker: expanded to
        // nothing it would leave a bare `{ ... }` at namespace scope. Guard it
        // instead, so the wrapped build simply has no block here.
        if (wrap_guard.empty()) {
          mods.push_back({begin_off, 0, "extern \"C++\" {\n"});
          mods.push_back({end_off, 0, "\n}"});
        } else {
          mods.push_back({begin_off, 0,
                          std::format("#ifndef {}\nextern \"C++\" {{\n#endif\n",
                                      wrap_guard)});
          mods.push_back({end_off, 0,
                          std::format("\n#ifndef {}\n}}\n#endif\n", wrap_guard)});
        }
      } else {
        // A definition already written `extern const TypeId x = ...;` carries
        // the storage-class specifier that `extern "C++"` makes redundant.
        // Drop the plain `extern` (a deletion, pushed before the insertion at
        // the same offset so the deletion applies first) and keep a clean
        // `extern "C++" const ...`.
        bool replaced = false;
        if (vd->getStorageClass() == clang::SC_Extern) {
          unsigned ex_start = 0, ex_end = 0;
          if (find_extern_spec(buf, begin_off, ex_start, ex_end)) {
            mods.push_back({ex_start, ex_end - ex_start, ""});
            mods.push_back({ex_start, 0, marker});
            replaced = true;
          }
        }
        if (!replaced) mods.push_back({begin_off, 0, marker});
        // `extern "C++"` acts as the extern storage-class specifier, so a bare
        // definition without an initializer (`int g_x;`) would become a mere
        // declaration. Keep it a definition by adding an empty initializer.
        // A static data member defined out-of-line still carries its in-class
        // initializer (e.g. `static const uint32_t kMaxRange = 1u << 31;`
        // defined as `const uint32_t Random::kMaxRange;`), so `{}` must not be
        // added — it would conflict with the in-class initializer.
        bool has_init = vd->hasInit();
        if (!has_init && vd->isStaticDataMember()) {
          if (auto *iv = vd->getCanonicalDecl()) has_init = iv->hasInit();
        }
        if (!has_init) {
          auto end = sm.getExpansionLoc(vd->getEndLoc());
          unsigned semi_off = sm.getFileOffset(end);
          while (semi_off < buf.size() && buf[semi_off] != ';') ++semi_off;
          if (semi_off < buf.size())
            mods.push_back({semi_off, 0, "{}"});
        }
      }
    }
    return true;
  }

  bool VisitCXXRecordDecl(clang::CXXRecordDecl *rd) {
    if (!rd || rd->isImplicit() || !rd->isThisDeclarationADefinition())
      return true;
    if (llvm::isa<clang::CXXRecordDecl>(rd->getDeclContext())) return true;
    if (llvm::isa<clang::FunctionDecl>(rd->getDeclContext())) return true;
    if (rd->getFriendObjectKind() != clang::Decl::FOK_None) return true;
    auto loc = rd->getLocation();
    if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
    if (!extern_classes.count(decl_fqn(rd))) return true;
    auto begin = sm.getExpansionLoc(rd->getBeginLoc());
    if (!begin.isValid()) return true;
    mods.push_back({sm.getFileOffset(begin), 0, marker});
    return true;
  }

private:
  bool is_cross_module(clang::NamedDecl *d) {
    auto fqn = decl_fqn(d);
    return fwd_declared_fqns.count(fqn) != 0;
  }

  // True when the class itself, or any class enclosing it, is extern "C++" (in
  // fwd_declared_fqns or the base-closure set). A member of a nested class
  // inside an extern "C++" class is itself a global-module entity.
  bool class_or_enclosing_cross_module(clang::CXXRecordDecl *cls) {
    for (auto *c = cls; c; c = llvm::dyn_cast<clang::CXXRecordDecl>(
             c->getDeclContext())) {
      auto fqn = decl_fqn(c);
      if (fwd_declared_fqns.count(fqn) || extern_classes.count(fqn))
        return true;
    }
    return false;
  }

  const clang::SourceManager &sm;
  const std::set<std::string> &fwd_declared_fqns;
  const std::set<std::string> &extern_classes;
  std::vector<ModPoint> &mods;
  std::string marker;
  std::string wrap_guard;
};

// Collects complete class definitions in an implementation file so the extern
// "C++" base-class closure can be computed (a global-module class cannot derive
// from a module-entity class, so the whole base chain must be extern "C++").
export class ImplClassDefCollector
    : public clang::RecursiveASTVisitor<ImplClassDefCollector> {
public:
  ImplClassDefCollector(const clang::SourceManager &sm,
                        std::map<std::string, clang::CXXRecordDecl *> &defs)
      : sm(sm), defs(defs) {}

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  bool VisitCXXRecordDecl(clang::CXXRecordDecl *rd) {
    if (!rd || rd->isImplicit() || !rd->isThisDeclarationADefinition())
      return true;
    if (llvm::isa<clang::CXXRecordDecl>(rd->getDeclContext())) return true;
    if (llvm::isa<clang::FunctionDecl>(rd->getDeclContext())) return true;
    if (rd->getFriendObjectKind() != clang::Decl::FOK_None) return true;
    auto loc = rd->getLocation();
    if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
    defs[decl_fqn(rd)] = rd;
    return true;
  }

private:
  const clang::SourceManager &sm;
  std::map<std::string, clang::CXXRecordDecl *> &defs;
};

export class ExternCxxDefsConsumer : public clang::ASTConsumer {
public:
  ExternCxxDefsConsumer(clang::ASTContext &ctx,
                        const std::set<std::string> &fwd_declared_fqns,
                        std::vector<ModPoint> &mods,
                        std::string marker = "extern \"C++\" ",
                        std::string wrap_guard = {})
      : ctx(ctx), fwd_declared_fqns(fwd_declared_fqns), mods(mods),
        marker(std::move(marker)), wrap_guard(std::move(wrap_guard)) {}

  void HandleTranslationUnit(clang::ASTContext &) override {
    std::map<std::string, clang::CXXRecordDecl *> class_defs;
    ImplClassDefCollector cdc(ctx.getSourceManager(), class_defs);
    cdc.TraverseDecl(ctx.getTranslationUnitDecl());

    // A class must be extern "C++" when it is forward-declared by another
    // module, or when it is (transitively) a base class of one: a global-module
    // class cannot have a module-entity base class, so externness propagates
    // from a derived class down through its bases.
    std::set<std::string> extern_classes;
    for (auto &[fqn, rd] : class_defs)
      if (fwd_declared_fqns.count(fqn)) extern_classes.insert(fqn);
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto &[fqn, rd] : class_defs) {
        if (!extern_classes.count(fqn)) continue;
        for (auto &b : rd->bases()) {
          if (auto *br = b.getType()->getAsCXXRecordDecl()) {
            std::string bfqn;
            for (auto *dc = br->getDeclContext(); dc; dc = dc->getParent()) {
              if (auto *nd = llvm::dyn_cast<clang::NamespaceDecl>(dc)) {
                if (!nd->isAnonymousNamespace())
                  bfqn = nd->getNameAsString() + "::" + bfqn;
              }
            }
            bfqn += br->getNameAsString();
            if (class_defs.count(bfqn) && !extern_classes.count(bfqn)) {
              extern_classes.insert(bfqn);
              changed = true;
            }
          }
        }
      }
    }

    ExternCxxDefCollector v(ctx.getSourceManager(), fwd_declared_fqns,
                            extern_classes, mods, marker, wrap_guard);
    v.TraverseDecl(ctx.getTranslationUnitDecl());
  }

private:
  clang::ASTContext &ctx;
  const std::set<std::string> &fwd_declared_fqns;
  std::vector<ModPoint> &mods;
  std::string marker;
  std::string wrap_guard;
};
