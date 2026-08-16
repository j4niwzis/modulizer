module;

export module modulizer.analyzer;
import modulizer.astutil;
import modulizer.rewrite_util;
import modulizer.util;
import libtooling;
import std;

export struct EntityMacro {
  std::string name;
  // The macro's full source text INCLUDING the `#define` keyword (e.g.
  // `#define FOO(a, b) a + b`), preserving the original formatting (function-
  // like parameter lists, variadic `...`, `\` line continuations). Emitted
  // verbatim into generated companion macro headers.
  std::string body;
  bool is_function_like = false;
  std::vector<std::string> params;
  std::vector<std::string> tokens;
};

export struct EntityItem {
  enum Kind { kClass, kStruct, kFunction, kEnum, kAlias, kVariable,
              kUsingDecl, kUsingDirective };

  Kind kind{};
  std::string name;
  std::vector<std::string> ns_path;
  std::string guard_prefix;  // e.g. "#ifdef _WIN32\n"
  std::string guard_suffix;  // e.g. "#endif // _WIN32\n"
  bool complete = true;      // false for forward declarations (no body)
  // True when the entity has C language linkage (declared inside `extern "C"`).
  // A header wrapper re-exports it from a separate `extern "C"` block: it
  // cannot sit in the `extern "C++"` one that carries everything else.
  bool c_language_linkage = false;
  // FQNs (namespace-qualified) of library types this entity's declaration
  // references. For a function: return + parameter types; for a class: base
  // classes + the return/parameter types of its public member functions; for a
  // variable/alias: its type / underlying type. Consumed by
  // expand_transitive_types to close reachability over type signatures.
  std::vector<std::string> type_refs;
  // True when the entity has internal or no linkage, which a wrapper cannot
  // re-export: an exported using-declaration may not name such an entity
  // ([module.interface]). Namespace-scope `const`/`constexpr` variables are
  // the common case (they are internal unless `inline` or `extern`), along
  // with the enumerators of an unnamed enum, which have no linkage at all.
  //
  // Only the re-export is impossible. Exporting the DECLARATION itself is
  // fine — that is a different path, and one that gives the entity external
  // linkage ([basic.link]) rather than naming it where it stands.
  bool no_external_linkage = false;
  // True when the entity's value is usable in constant expressions, so a
  // wrapper can hand consumers a copy of it under the same name even though it
  // cannot name the original. Only meaningful together with
  // no_external_linkage: an entity that has a linkage to name is named.
  bool constant_value = false;
};

export struct EntityModel {
  std::vector<EntityItem> items;
  std::vector<EntityMacro> macros;
};

export constexpr llvm::StringRef kDefaultInternalFilter =
    "^(detail|internal|impl|_.*)$";

namespace {

struct GuardRange {
  unsigned begin;
  unsigned end;
  std::string prefix;
  std::string suffix;
};

// Namespace-only qualified name matching the tool's FQN convention
// (fqn_of(ns_path, name)). Class/function nesting is dropped because the
// entity model only holds namespace-scope entities.
std::string ns_fqn(clang::NamedDecl *d) {
  if (!d) return {};
  std::string out;
  for (auto *dc = d->getDeclContext(); dc; dc = dc->getParent()) {
    if (auto *nd = llvm::dyn_cast<clang::NamespaceDecl>(dc)) {
      if (!nd->isAnonymousNamespace() && !nd->isInline())
        out = nd->getNameAsString() + "::" + out;
    }
  }
  out += d->getNameAsString();
  return out;
}

// Collect the FQNs of library entities named by a type. Only namespace-scope
// entities are recorded; template parameters, injected class names, dependent
// members and builtins yield nothing. This is the type detail behind
// expand_transitive_types: a reachable function exposes its return/parameter
// types, a reachable variable its type, an alias its underlying type, and a
// reachable class the types its public member functions mention.
void collect_type_fqns(clang::QualType qt, std::set<std::string> &out,
                       int depth = 0) {
  if (qt.isNull() || depth > 60) return;
  auto *t = qt.getTypePtrOrNull();
  if (!t) return;
  // Peel parenthesized and array-decayed sugar that names nothing.
  if (auto *pt = llvm::dyn_cast<clang::ParenType>(t)) {
    collect_type_fqns(pt->getInnerType(), out, depth + 1);
    return;
  }
  if (auto *dt = llvm::dyn_cast<clang::DecayedType>(t)) {
    collect_type_fqns(dt->getDecayedType(), out, depth + 1);
    return;
  }
  if (auto *tt = t->getAs<clang::TypedefType>()) {
    // Record the alias/typedef entity itself, then expand its underlying type.
    out.insert(ns_fqn(tt->getDecl()));
    collect_type_fqns(tt->desugar(), out, depth + 1);
    return;
  }
  if (auto *at = llvm::dyn_cast<clang::ArrayType>(t)) {
    collect_type_fqns(at->getElementType(), out, depth + 1);
    return;
  }
  if (auto *ft = llvm::dyn_cast<clang::FunctionProtoType>(t)) {
    collect_type_fqns(ft->getReturnType(), out, depth + 1);
    for (auto &pt : ft->getParamTypes())
      collect_type_fqns(pt, out, depth + 1);
    return;
  }
  if (t->isReferenceType() || t->isPointerType() || t->isMemberPointerType()) {
    collect_type_fqns(t->getPointeeType(), out, depth + 1);
    return;
  }
  if (auto *tst = llvm::dyn_cast<clang::TemplateSpecializationType>(t)) {
    // Record the class/alias template, then each type argument.
    if (auto *td = tst->getTemplateName().getAsTemplateDecl())
      out.insert(ns_fqn(td));
    for (auto &arg : tst->template_arguments()) {
      if (arg.getKind() == clang::TemplateArgument::Type)
        collect_type_fqns(arg.getAsType(), out, depth + 1);
      else if (arg.getKind() == clang::TemplateArgument::Pack)
        for (auto sub : arg.pack_elements())
          if (sub.getKind() == clang::TemplateArgument::Type)
            collect_type_fqns(sub.getAsType(), out, depth + 1);
    }
    return;
  }
  if (auto *stpt = llvm::dyn_cast<clang::SubstTemplateTypeParmType>(t)) {
    collect_type_fqns(stpt->getReplacementType(), out, depth + 1);
    return;
  }
  if (auto *dt = llvm::dyn_cast<clang::DecltypeType>(t)) {
    // `decltype(detail::Foo{})` names a type through an expression; walk the
    // operand for type declarations and constructor calls. Function/variable
    // references (e.g. `decltype(detail::compute())`) are NOT recorded here —
    // those are the job of the cross-module template-body analysis, and
    // exporting them would change their shared-entity `extern "C++"` treatment.
    if (auto *expr = dt->getUnderlyingExpr()) {
      struct TypeScanner
          : public clang::RecursiveASTVisitor<TypeScanner> {
        std::set<std::string> &out;
        explicit TypeScanner(std::set<std::string> &out) : out(out) {}
        bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
          if (!e) return true;
          auto *d = e->getDecl();
          if (!d) return true;
          if (llvm::isa<clang::CXXRecordDecl>(d) ||
              llvm::isa<clang::TypeAliasDecl>(d) ||
              llvm::isa<clang::TypedefDecl>(d) ||
              llvm::isa<clang::EnumDecl>(d) ||
              llvm::isa<clang::ClassTemplateDecl>(d) ||
              llvm::isa<clang::TypeAliasTemplateDecl>(d))
            out.insert(ns_fqn(d));
          return true;
        }
        bool VisitCXXConstructExpr(clang::CXXConstructExpr *e) {
          if (!e) return true;
          if (auto *ctor = e->getConstructor())
            if (auto *cls =
                    llvm::dyn_cast<clang::CXXRecordDecl>(ctor->getParent()))
              out.insert(ns_fqn(cls));
          return true;
        }
      } ts(out);
      ts.TraverseStmt(expr);
    }
    return;
  }
  if (auto *dnt = llvm::dyn_cast<clang::DependentNameType>(t)) {
    // `typename T::value_type` — the qualifier may be a concrete type whose
    // own declaration mentions further entities.
    auto nns = dnt->getQualifier();
    if (nns.getKind() == clang::NestedNameSpecifier::Kind::Type)
      if (auto *q = nns.getAsType())
        collect_type_fqns(clang::QualType(q, 0), out, depth + 1);
    return;
  }
  // Template parameters and injected class names are not concrete entities;
  // everything else that names a tag (class/struct/enum) is recorded.
  if (auto *td = t->getAsTagDecl()) {
    out.insert(ns_fqn(td));
    return;
  }
}

// The type FQNs a function declaration exposes: its return type and the types
// of all parameters.
void collect_function_type_refs(clang::FunctionDecl *fd,
                                std::set<std::string> &out) {
  if (!fd) return;
  collect_type_fqns(fd->getReturnType(), out);
  for (auto *p : fd->parameters())
    if (p) collect_type_fqns(p->getType(), out);
}

class EntityVisitor : public NsPathVisitor<EntityVisitor> {
public:
  EntityVisitor(clang::ASTContext &ctx, EntityModel &model,
                const std::vector<GuardRange> &guard_ranges)
      : ctx(ctx), model(model), guard_ranges(guard_ranges) {}

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  bool is_internal_ns(const std::string &) const { return false; }

  bool VisitCXXRecordDecl(clang::CXXRecordDecl *rd) {
    if (!rd || !isMainFile(rd)) return true;
    if (rd->isImplicit()) return true;
    // A friend class forward declaration inside another class (e.g.
    // `friend class internal::Factory;`) is a namespace-scope
    // entity: it forward-declares a class that another module defines. Its
    // lexical position is inside the friend-declaring class, so the traversal
    // ns_path_ is wrong; use the semantic DeclContext chain instead. Recording
    // it lets cross-module analysis treat it as a cross-module forward
    // declaration (hence `extern "C++"` on both sides and an `extern "C++"`
    // re-export in the wrapper).
    bool is_friend = rd->getFriendObjectKind() != clang::Decl::FOK_None;
    if (auto *ct = rd->getDescribedClassTemplate())
      if (ct->getFriendObjectKind() != clang::Decl::FOK_None) is_friend = true;
    std::vector<std::string> path = ns_path_;
    if (is_friend) {
      if (!is_namespace_scope(rd)) return true;
      // A friend declaration is only a namespace-scope entity when the target
      // is actually DEFINED in this TU (e.g. `friend class
      // internal::Factory;`, whose defining header is
      // included). A friend whose target is never defined — a phantom, or a
      // class living in a transitive header that is not part of the analyzed
      // library (clang's `friend class ODRDiagsEmitter;`, `friend class
      // TemplateDeclInstantiator;`) — would produce an invalid
      // `using ::ns::Name;` in the wrapper.
      if (!rd->isCompleteDefinition()) return true;
      path.clear();
      for (auto *dc = rd->getDeclContext(); dc; dc = dc->getParent()) {
        if (auto *nd = llvm::dyn_cast<clang::NamespaceDecl>(dc)) {
          if (!nd->isAnonymousNamespace() && !nd->isInline())
            path.insert(path.begin(), nd->getNameAsString());
        }
      }
    } else if (!is_namespace_scope(rd)) {
      return true;
    }
    // An explicit specialization (`template <> struct Foo<Bar> { ... }`) is not
    // a separate namespace-scope name: the primary template is the entity, and
    // a `using ::ns::Foo;` for the specialization is invalid (clang's
    // `template <> struct llvm::DenseMapInfo<FoldingSetNodeID>` at global
    // scope would yield `using ::DenseMapInfo;`). The primary template is
    // recorded from its own header.
    if (rd->getTemplateSpecializationKind() ==
        clang::TemplateSpecializationKind::TSK_ExplicitSpecialization)
      return true;
    auto [it, inserted] = seen.insert(rd);
    if (!inserted) return true;

    auto kind = rd->isStruct()
                    ? EntityItem::kStruct
                    : EntityItem::kClass;
    EntityItem item{kind, rd->getNameAsString(), path};
    item.complete = rd->isCompleteDefinition();
    std::set<std::string> refs;
    if (rd->isCompleteDefinition()) {
      // A reachable type exposes the types its base classes and public member
      // functions mention: consumers can name them through the type.
      for (auto &b : rd->bases())
        collect_type_fqns(b.getType(), refs);
      for (auto *md : rd->methods()) {
        if (md->getAccess() != clang::AS_public) continue;
        collect_function_type_refs(md, refs);
      }
    }
    item.type_refs = std::ranges::to<std::vector>(refs);
    add_item(std::move(item), rd);
    return true;
  }

  bool VisitFunctionDecl(clang::FunctionDecl *fd) {
    if (!fd || !isMainFile(fd)) return true;
    if (fd->isImplicit()) return true;
    if (!is_namespace_scope(fd)) return true;
    if (llvm::isa<clang::CXXMethodDecl>(fd)) return true;
    // A friend function declared inside a class (e.g.
    // `friend internal::Impl* internal::GetImpl();`) has the
    // enclosing namespace as its semantic DeclContext, but it is not a
    // namespace-scope entity: the wrapper must not re-export it.
    if (fd->getFriendObjectKind() != clang::Decl::FOK_None) return true;
    // A namespace-scope `static` function (or an anonymous-namespace member)
    // has internal linkage and can never be re-exported via
    // `using ::ns::Name;` (clang's
    // `static constexpr StringRef getOpenMPVariantManglingSeparatorStr()`).
    if (fd->getFormalLinkage() != clang::Linkage::External) return true;
    auto [it, inserted] = seen.insert(fd);
    if (!inserted) return true;

    EntityItem item{EntityItem::kFunction, fd->getNameAsString(), ns_path_};
    item.complete = fd->isThisDeclarationADefinition();
    std::set<std::string> refs;
    collect_function_type_refs(fd, refs);
    item.type_refs = std::ranges::to<std::vector>(refs);
    add_item(std::move(item), fd);
    return true;
  }

  bool VisitFunctionTemplateDecl(clang::FunctionTemplateDecl *ft) {
    if (!ft || !isMainFile(ft)) return true;
    if (ft->isImplicit()) return true;
    if (!is_namespace_scope(ft)) return true;
    auto [it, inserted] = seen.insert(ft);
    if (!inserted) return true;

    // A function template is only "complete" when it has a body. A bare
    // declaration (`template <typename T> std::string print_string(const T&);`)
    // is a forward declaration: cross_module_fwd_declared_fqns must flag it so
    // the definition in another header gets the shared `extern "C++"` treatment
    // (the entity is used across modules). Defined templates (e.g. `call`) stay
    // complete so cross-module template-body analyzers can match them.
    EntityItem item{EntityItem::kFunction, ft->getNameAsString(), ns_path_};
    item.complete = ft->getTemplatedDecl()->doesThisDeclarationHaveABody();
    std::set<std::string> refs;
    collect_function_type_refs(ft->getTemplatedDecl(), refs);
    item.type_refs = std::ranges::to<std::vector>(refs);
    add_item(std::move(item), ft);
    return true;
  }

  bool VisitEnumDecl(clang::EnumDecl *ed) {
    if (!ed || !isMainFile(ed)) return true;
    if (ed->isImplicit()) return true;
    if (!is_namespace_scope(ed)) return true;
    // An anonymous enum has no name to re-export (`using ::clang::;` is
    // invalid); its enumerators are recorded individually by
    // VisitEnumConstantDecl.
    if (ed->getNameAsString().empty()) return true;
    auto [it, inserted] = seen.insert(ed);
    if (!inserted) return true;

    add_item({EntityItem::kEnum, ed->getNameAsString(), ns_path_}, ed);
    return true;
  }

  bool VisitEnumConstantDecl(clang::EnumConstantDecl *ecd) {
    if (!ecd || ecd->isImplicit()) return true;
    // Only UN-scoped enum enumerators are injected into the enclosing namespace
    // (e.g. `clang::AS_public` from `enum AccessSpecifier`), so the wrapper can
    // re-export them via `using ::clang::AS_public;`. Scoped-enum (`enum class`)
    // enumerators live inside the enum type (`clang::Linkage::External`) and
    // are reached through the re-exported enum itself.
    auto *ed = llvm::dyn_cast<clang::EnumDecl>(ecd->getDeclContext());
    if (!ed || ed->isScoped()) return true;
    // The ENUM must be in the analyzed header — its enumerators belong to it
    // even when their spelling lives in a `.def` file included by the enum
    // (clang's `clang::tok::eof` comes from TokenKinds.def via `#define TOK`).
    if (!isMainFile(ed)) return true;
    if (!is_namespace_scope(ed)) return true;
    auto [it, inserted] = seen.insert(ecd);
    if (!inserted) return true;

    add_item({EntityItem::kEnum, ecd->getNameAsString(), ns_path_}, ecd);
    return true;
  }

  bool VisitTypedefDecl(clang::TypedefDecl *td) {
    if (!td || !isMainFile(td)) return true;
    if (td->isImplicit()) return true;
    if (!is_namespace_scope(td)) return true;
    auto [it, inserted] = seen.insert(td);
    if (!inserted) return true;

    EntityItem item{EntityItem::kAlias, td->getNameAsString(), ns_path_};
    std::set<std::string> refs;
    collect_type_fqns(td->getUnderlyingType(), refs);
    item.type_refs = std::ranges::to<std::vector>(refs);
    add_item(std::move(item), td);
    return true;
  }

  bool VisitTypeAliasDecl(clang::TypeAliasDecl *ta) {
    if (!ta || !isMainFile(ta)) return true;
    if (ta->isImplicit()) return true;
    if (!is_namespace_scope(ta)) return true;
    auto [it, inserted] = seen.insert(ta);
    if (!inserted) return true;

    EntityItem item{EntityItem::kAlias, ta->getNameAsString(), ns_path_};
    std::set<std::string> refs;
    collect_type_fqns(ta->getUnderlyingType(), refs);
    item.type_refs = std::ranges::to<std::vector>(refs);
    add_item(std::move(item), ta);
    return true;
  }

  bool VisitTypeAliasTemplateDecl(clang::TypeAliasTemplateDecl *tat) {
    if (!tat || !isMainFile(tat)) return true;
    if (tat->isImplicit()) return true;
    if (!is_namespace_scope(tat)) return true;
    auto [it, inserted] = seen.insert(tat);
    if (!inserted) return true;

    // `template <typename T> using X = ...;` — record the alias template as a
    // complete entity so cross-module template-body analyzers can match it.
    EntityItem item{EntityItem::kAlias, tat->getNameAsString(), ns_path_};
    item.complete = true;
    std::set<std::string> refs;
    if (auto *ta = tat->getTemplatedDecl())
      collect_type_fqns(ta->getUnderlyingType(), refs);
    item.type_refs = std::ranges::to<std::vector>(refs);
    add_item(std::move(item), tat);
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *vd) {
    if (!vd || !isMainFile(vd)) return true;
    if (vd->isImplicit()) return true;
    if (!is_namespace_scope(vd)) return true;
    if (vd->isLocalVarDeclOrParm()) return true;
    if (vd->isStaticDataMember()) return true;
    auto [it, inserted] = seen.insert(vd);
    if (!inserted) return true;

    EntityItem item{EntityItem::kVariable, vd->getNameAsString(), ns_path_};
    std::set<std::string> refs;
    collect_type_fqns(vd->getType(), refs);
    item.type_refs = std::ranges::to<std::vector>(refs);
    add_item(std::move(item), vd);
    return true;
  }

  bool VisitUsingDecl(clang::UsingDecl *ud) {
    // A namespace-scope `using X::Y;` declaration (e.g. a library's
    // `using internal::Foo;`) hoists a name into the enclosing namespace
    // for consumers. It is itself a re-exportable entity: the wrapper must
    // re-export `using ::lib::Foo;` so `import <lib>;` gives consumers
    // the alias. Class- and function-scope using-declarations are not entities.
    if (!ud || !isMainFile(ud)) return true;
    if (ud->isImplicit()) return true;
    if (llvm::isa<clang::CXXRecordDecl>(ud->getDeclContext())) return true;
    if (llvm::isa<clang::FunctionDecl>(ud->getDeclContext())) return true;
    if (!llvm::isa<clang::NamespaceDecl>(ud->getDeclContext())) return true;
    auto [it, inserted] = seen.insert(ud);
    if (!inserted) return true;

    EntityItem item{EntityItem::kUsingDecl, ud->getNameAsString(), ns_path_};
    add_item(std::move(item), ud);
    return true;
  }

  bool VisitUsingDirectiveDecl(clang::UsingDirectiveDecl *udd) {
    // A namespace-scope `using namespace X;` directive (e.g. a library's
    // `using namespace detail;` inside `namespace lib`) makes X's members
    // visible in the enclosing namespace. It must be re-exported so consumers
    // resolve names like `lib::Foo` through it.
    if (!udd || !isMainFile(udd)) return true;
    if (udd->isImplicit()) return true;
    if (llvm::isa<clang::CXXRecordDecl>(udd->getDeclContext())) return true;
    if (llvm::isa<clang::FunctionDecl>(udd->getDeclContext())) return true;
    auto [it, inserted] = seen.insert(udd);
    if (!inserted) return true;

    std::string target;
    if (auto *ns = udd->getNominatedNamespace())
      target = ns->getNameAsString();
    EntityItem item{EntityItem::kUsingDirective, target, ns_path_};
    add_item(std::move(item), udd);
    return true;
  }

private:
  clang::ASTContext &ctx;
  EntityModel &model;
  std::set<const void *> seen;
  const std::vector<GuardRange> &guard_ranges;

  bool isMainFile(clang::Decl *d) {
    if (!d) return false;
    auto loc = d->getLocation();
    return loc.isValid() && ctx.getSourceManager().isInMainFile(loc);
  }

  // Only namespace- (or global-) scope declarations are entities. Class-scope
  // declarations (nested typedefs, member typedefs, friend functions) would
  // otherwise be recorded with the enclosing namespace path and the wrapper
  // would re-export invalid `using ::ns::Name;` declarations for them.
  void apply_guard(EntityItem &item, clang::Decl *d) {
    if (guard_ranges.empty()) return;
    auto off = ctx.getSourceManager().getFileOffset(d->getLocation());
    for (auto &r : guard_ranges) {
      if (off >= r.begin && off < r.end) {
        item.guard_prefix = r.prefix;
        item.guard_suffix = r.suffix;
        return;
      }
    }
  }

  void add_item(EntityItem item, clang::Decl *d) {
    apply_guard(item, d);
    // isExternC() lives on the declarations that can have language linkage.
    if (auto *fd = llvm::dyn_cast_or_null<clang::FunctionDecl>(d))
      item.c_language_linkage = fd->isExternC();
    else if (auto *vd = llvm::dyn_cast_or_null<clang::VarDecl>(d))
      item.c_language_linkage = vd->isExternC();
    // Asked only of the kinds that reach namespace scope without external
    // linkage: a `static` function, a `const`/`constexpr` variable, or an
    // enumerator of an unnamed enum. A class name has external linkage by
    // construction and a type alias has no linkage of its own to compute, so
    // asking about either would only invite a wrong answer.
    if (auto *nd = llvm::dyn_cast_or_null<clang::NamedDecl>(d);
        nd && (llvm::isa<clang::VarDecl>(d) ||
               llvm::isa<clang::EnumConstantDecl>(d) ||
               llvm::isa<clang::FunctionDecl>(d)))
      item.no_external_linkage =
          nd->getFormalLinkage() != clang::Linkage::External;
    // An enumerator is a constant; a variable is one only if it was declared
    // in a way that lets its value be read at compile time. Anything else
    // (a `static` mutable global, a `static` function) has no value to copy.
    if (llvm::isa_and_nonnull<clang::EnumConstantDecl>(d))
      item.constant_value = true;
    else if (auto *vd = llvm::dyn_cast_or_null<clang::VarDecl>(d))
      item.constant_value = vd->isUsableInConstantExpressions(ctx);
    model.items.push_back(std::move(item));
  }
};

class MacroCollector : public clang::PPCallbacks {
public:
  MacroCollector(clang::SourceManager &sm, EntityModel &model,
                 bool wrapper_mode, std::set<std::string> *undefed)
      : sm(sm), model(model), wrapper_mode(wrapper_mode), undefed(undefed) {}

  void MacroDefined(const clang::Token &name,
                    const clang::MacroDirective *md) override {
    auto *mi = collectible_macro(md, sm);
    if (!mi) return;
    auto defLoc = mi->getDefinitionLoc();

    // Capture the macro's full source text verbatim (from the `#` to the end
    // of the definition), so generated companion headers reproduce the
    // original `#define` exactly — including function-like parameter lists,
    // variadic `...`, and `\` line continuations — instead of a
    // name-then-tokens reconstruction that would be invalid C++.
    auto fileID = sm.getFileID(defLoc);
    unsigned nameOff = sm.getFileOffset(defLoc);
    const char *buf = sm.getCharacterData(sm.getLocForStartOfFile(fileID));
    unsigned hashOff = nameOff;
    while (hashOff > 0 && buf[hashOff - 1] != '#') --hashOff;
    --hashOff;
    auto defEnd = mi->getDefinitionEndLoc();
    unsigned endLine = sm.getExpansionLineNumber(defEnd);
    auto nextLine = sm.translateLineCol(fileID, endLine + 1, 1);
    unsigned endOff = nextLine.isValid()
        ? sm.getFileOffset(nextLine)
        : sm.getFileOffset(sm.getLocForEndOfFile(fileID));
    std::string body(buf + hashOff, buf + endOff);

    auto macro_name = std::string(name.getIdentifierInfo()->getName());

    // In wrapper mode, drop include-guard defines clang did not classify as
    // guards (each header is parsed as the main file, so isUsedForHeaderGuard
    // is not set for them): a `#define X` immediately following a
    // `#ifndef X` whose name looks like a guard. Guard macros are meaningless
    // in a companion macro header (there is nothing to guard).
    if (wrapper_mode) {
      std::string_view before(buf, hashOff);
      auto nl = before.rfind('\n');
      if (nl != std::string_view::npos && nl > 0) {
        auto pnl = before.rfind('\n', nl - 1);
        auto start = pnl == std::string_view::npos ? 0 : pnl + 1;
        auto prev = before.substr(start, nl - start);
        if (prev.starts_with("#ifndef") &&
            prev.find(macro_name) != std::string_view::npos &&
            looks_like_guard_name(macro_name))
          return;
      }
    }

    std::vector<std::string> tokens;
    clang::LangOptions lo;
    for (unsigned i = 0; i < mi->getNumTokens(); ++i)
      tokens.push_back(std::string(clang::Lexer::getSpelling(
          mi->getReplacementToken(i), sm, lo)));

    bool fun = mi->isFunctionLike();
    std::vector<std::string> params;
    if (fun) {
      for (auto it = mi->param_begin(); it != mi->param_end(); ++it)
        params.push_back((*it)->getName().str());
    }

    model.macros.push_back(
        {std::move(macro_name), body, fun, std::move(params),
         std::move(tokens)});
  }

  // In wrapper mode, record every `#undef` seen while parsing the wrapper's
  // headers — in the current file or in any file it includes (e.g. the private
  // `BuiltinTemplate`/`IMAGE_TYPE` implementation macros are defined in a
  // wrapper header but `#undef`'d in an included `.inc`/`.def`). Macros that
  // any TU of the batch `#undef`s are private implementation macros, not part
  // of the re-exported API surface, so analyze_headers_with_cdb drops them
  // from the companion header. Undefs of system macros are harmless: those
  // names are never collected as library macros.
  void MacroUndefined(const clang::Token &name,
                      const clang::MacroDefinition &,
                      const clang::MacroDirective *) override {
    if (!wrapper_mode || !undefed) return;
    auto id = name.getIdentifierInfo();
    if (!id) return;
    undefed->insert(id->getName().str());
  }

private:
  clang::SourceManager &sm;
  EntityModel &model;
  bool wrapper_mode;
  std::set<std::string> *undefed;
};

class GuardTracker : public clang::PPCallbacks {
public:
  GuardTracker(clang::SourceManager &sm,
               std::vector<GuardRange> &ranges,
               std::vector<std::string> &ifdef_macros)
      : sm(sm), ranges(ranges), ifdef_macros(ifdef_macros) {}

  void Ifdef(clang::SourceLocation loc, const clang::Token &name,
             const clang::MacroDefinition &) override {
    pending_file_guard.clear();
    if (sm.isInMainFile(loc) && name.getIdentifierInfo()) {
      auto macro = name.getIdentifierInfo()->getName().str();
      ifdef_macros.push_back(macro);
      stack.push_back({loc, std::move(macro), false, false, false});
    }
  }

  void Ifndef(clang::SourceLocation loc, const clang::Token &name,
              const clang::MacroDefinition &) override {
    if (sm.isInMainFile(loc) && name.getIdentifierInfo()) {
      auto macro = name.getIdentifierInfo()->getName().str();
      // The header's own include guard (`#ifndef X` ... `#define X` at the top
      // of the file) must NOT wrap entities: in the wrapper module the GMF
      // includes the header, so the guard macro is already defined and a
      // `#ifndef X` re-export block would evaluate false, dropping every
      // entity of that header. A top-level `#ifndef` whose macro name is
      // `#define`d immediately after (confirmed in MacroDefined) is the file
      // guard.
      if (stack.empty() && pending_file_guard.empty())
        pending_file_guard = macro;
      stack.push_back({loc, std::move(macro), true, false, false});
    }
  }

  void If(clang::SourceLocation loc, clang::SourceRange,
          clang::PPCallbacks::ConditionValueKind) override {
    pending_file_guard.clear();
    if (sm.isInMainFile(loc))
      stack.push_back({loc, {}, false, false, false, condition_text(loc)});
  }

  void MacroDefined(const clang::Token &name,
                    const clang::MacroDirective *) override {
    if (!pending_file_guard.empty() && !stack.empty() &&
        name.getIdentifierInfo() &&
        pending_file_guard == name.getIdentifierInfo()->getName().str())
      stack.back().is_file_guard = true;
    pending_file_guard.clear();
  }

  void Else(clang::SourceLocation loc, clang::SourceLocation) override {
    if (stack.empty()) return;
    auto &f = stack.back();
    if (f.macro.empty() && f.cond.empty()) return;
    push_range(f, loc); // close range before #else
    f.loc = loc;
    f.flipped = !f.flipped; // flip guard sense for #else branch
  }

  void Endif(clang::SourceLocation loc,
             clang::SourceLocation) override {
    if (stack.empty()) return;
    auto f = stack.back();
    stack.pop_back();
    if (f.macro.empty() && f.cond.empty()) return;
    push_range(f, loc);
  }

  void Elif(clang::SourceLocation, clang::SourceRange,
            clang::PPCallbacks::ConditionValueKind,
            clang::SourceLocation) override {}

  static void merge_adjacent(std::vector<GuardRange> &ranges) {
    std::ranges::sort(ranges, [](const GuardRange &a, const GuardRange &b) {
      return a.begin < b.begin;
    });
    for (std::size_t i = 0; i + 1 < ranges.size(); ) {
      if (ranges[i].prefix == ranges[i + 1].prefix &&
          ranges[i].end == ranges[i + 1].begin) {
        ranges[i].end = ranges[i + 1].end;
        ranges.erase(ranges.begin() + i + 1);
      } else {
        ++i;
      }
    }
  }

private:
  struct Frame {
    clang::SourceLocation loc;
    std::string macro;
    bool is_ifndef;
    bool flipped = false;
    bool is_file_guard = false;
    // Verbatim condition of an `#if <expr>` (empty for #ifdef/#ifndef, whose
    // condition is rebuilt from `macro`). An entity declared only under such a
    // condition has to be re-exported under it too, or a wrapper names
    // something that does not exist in every configuration —
    // `testing::WhenDynamicCastTo` exists only `#if GTEST_HAS_RTTI`.
    std::string cond;
  };

  // Text of the `#if` condition at `loc`: from the directive to end of line,
  // with backslash continuations joined and the `#if` keyword removed.
  std::string condition_text(clang::SourceLocation loc) {
    auto buf = sm.getBufferData(sm.getFileID(loc));
    std::size_t off = sm.getFileOffset(loc);
    std::string out;
    while (off < buf.size()) {
      auto eol = buf.find('\n', off);
      if (eol == llvm::StringRef::npos) eol = buf.size();
      auto line = buf.substr(off, eol - off);
      bool cont = !line.empty() && line.back() == '\\';
      if (cont) line = line.drop_back(1);
      out.append(line.data(), line.size());
      off = eol + 1;
      if (!cont) break;
      out += ' ';
    }
    // Strip a leading `#` and the `if` keyword, wherever the location started.
    auto ns = out.find_first_not_of(" \t");
    if (ns != std::string::npos && out[ns] == '#') ns = out.find_first_not_of(" \t", ns + 1);
    if (ns != std::string::npos && out.compare(ns, 2, "if") == 0) ns += 2;
    out = ns == std::string::npos ? std::string{} : out.substr(ns);
    auto first = out.find_first_not_of(" \t");
    out = first == std::string::npos ? std::string{} : out.substr(first);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' ||
                            out.back() == '\r'))
      out.pop_back();
    return out;
  }

  void push_range(const Frame &f, clang::SourceLocation end) {
    // The file's own include guard covers the whole file; wrapping its
    // entities in `#ifndef X` would be dead code in the wrapper module (the
    // GMF already defines X by including the header).
    if (f.is_file_guard) return;
    auto fileID = sm.getFileID(f.loc);
    auto buf = sm.getBufferData(fileID);
    auto raw = sm.getFileOffset(f.loc);
    auto lineEnd = raw;
    while (lineEnd < buf.size() && buf[lineEnd] != '\n') ++lineEnd;
    auto beginOff = static_cast<unsigned>(lineEnd + 1);
    auto endOff = sm.getFileOffset(end);
    std::string prefix, suffix;
    if (!f.cond.empty()) {
      prefix = f.flipped ? std::format("#if !({})\n", f.cond)
                         : std::format("#if {}\n", f.cond);
      suffix = std::format("#endif // {}\n", f.cond);
    } else {
      bool cond = f.is_ifndef ^ f.flipped;
      prefix = std::format("{} {}\n", cond ? "#ifndef" : "#ifdef", f.macro);
      suffix = std::format("#endif // {}\n", f.macro);
    }
    ranges.push_back({beginOff, endOff, std::move(prefix), std::move(suffix)});
  }

  clang::SourceManager &sm;
  std::vector<GuardRange> &ranges;
  std::vector<std::string> &ifdef_macros;
  std::vector<Frame> stack;
  std::string pending_file_guard;
};

class EntityExtractionConsumer : public clang::ASTConsumer {
public:
  EntityExtractionConsumer(clang::ASTContext &ctx, EntityModel &model,
                           std::vector<GuardRange> &guard_ranges)
      : ctx(ctx), model(model), guard_ranges(guard_ranges) {}

  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    GuardTracker::merge_adjacent(guard_ranges);
    EntityVisitor visitor(ctx, model, guard_ranges);
    visitor.TraverseDecl(ctx.getTranslationUnitDecl());
  }

private:
  clang::ASTContext &ctx;
  EntityModel &model;
  std::vector<GuardRange> &guard_ranges;
};

class EntityExtractionAction : public clang::ASTFrontendAction {
public:
  EntityExtractionAction(EntityModel &model,
                         std::vector<std::string> &ifdef_macros,
                         bool wrapper_mode,
                         std::set<std::string> *undefed,
                         VisitorFrontendActionFactory::ConsumerMaker extra)
      : model(model), ifdef_macros(ifdef_macros),
        wrapper_mode(wrapper_mode), undefed(undefed),
        extra(std::move(extra)) {}

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
      clang::CompilerInstance &ci, llvm::StringRef) override {
    auto &pp = ci.getPreprocessor();
    auto &sm = ci.getSourceManager();
    pp.addPPCallbacks(
        std::make_unique<MacroCollector>(sm, model, wrapper_mode, undefed));
    pp.addPPCallbacks(
        std::make_unique<GuardTracker>(sm, guard_ranges, ifdef_macros));
    auto consumer = std::make_unique<EntityExtractionConsumer>(
        ci.getASTContext(), model, guard_ranges);
    if (!extra) return consumer;
    // Another analysis rides this parse rather than sweeping the same files
    // again; the parse is what costs, the traversal is not.
    std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
    consumers.push_back(std::move(consumer));
    consumers.push_back(extra(ci));
    return make_combined_consumer(std::move(consumers));
  }

private:
  EntityModel &model;
  std::vector<GuardRange> guard_ranges;
  std::vector<std::string> &ifdef_macros;
  bool wrapper_mode;
  std::set<std::string> *undefed;
  VisitorFrontendActionFactory::ConsumerMaker extra;
};

} // namespace

// Factory shared with modulizer.cross_module, which re-parses headers to
// compute cross-module FQN sets. Exported (not in the anonymous namespace)
// because the cross-module module instantiates it via parallel_parse.
// `wrapper_mode` enables the legacy `--wrapper` filtering (include-guard and
// cross-header `#undef` dropping); `undefed` is the shared set of `#undef`'d
// macro names accumulated across every TU of the batch.
export class EntityExtractionFactory : public clang::tooling::FrontendActionFactory {
public:
  // `extra`, when set, builds one more ASTConsumer to run over the same parse.
  explicit EntityExtractionFactory(
      EntityModel &model, std::vector<std::string> &ifdef_macros,
      bool wrapper_mode = false, std::set<std::string> *undefed = nullptr,
      VisitorFrontendActionFactory::ConsumerMaker extra = {})
      : model(model), ifdef_macros(ifdef_macros),
        wrapper_mode(wrapper_mode), undefed(undefed), extra(std::move(extra)) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<EntityExtractionAction>(model, ifdef_macros,
                                                    wrapper_mode, undefed,
                                                    extra);
  }

private:
  EntityModel &model;
  std::vector<std::string> &ifdef_macros;
  bool wrapper_mode;
  std::set<std::string> *undefed;
  VisitorFrontendActionFactory::ConsumerMaker extra;
};

export EntityModel analyze_headers_with_cdb(
    const clang::tooling::CompilationDatabase &cdb,
    const std::vector<std::string> &source_paths,
    bool wrapper_mode = false) {
  EntityModel model;
  std::vector<std::string> ifdef_macros;
  std::set<std::string> undefed;
  clang::tooling::ClangTool tool(cdb, source_paths);
  EntityExtractionFactory factory(model, ifdef_macros, wrapper_mode, &undefed);
  tool.run(&factory);
  if (wrapper_mode) {
    // Drop macros any header in the batch `#undef`'d — whether at the end of
    // their own file or in another header of the batch: they are private
    // implementation macros, not part of the re-exported API surface.
    std::erase_if(model.macros, [&](const EntityMacro &m) {
      return undefed.count(m.name) != 0;
    });
  }
  return model;
}

export EntityModel analyze_file(llvm::StringRef path) {
  EntityModel model;
  std::vector<std::string> sources = {path.str()};
  auto base_flags = base_compile_flags();

  // Pass 1: parse normally, collect entities from active branches
  std::vector<std::string> ifdef_macros;
  {
    clang::tooling::FixedCompilationDatabase db(".", base_flags);
    clang::tooling::ClangTool tool(db, sources);
    EntityExtractionFactory factory(model, ifdef_macros);
    tool.run(&factory);
  }

  // Pass 2: for each #ifdef macro, re-parse with only that macro defined.
  // Separate passes avoid conflicts between mutually exclusive features.
  std::ranges::sort(ifdef_macros);
  ifdef_macros.erase(std::ranges::unique(ifdef_macros).begin(),
                     ifdef_macros.end());
  for (auto &macro : ifdef_macros) {
    EntityModel model2;
    std::vector<std::string> ifdef_macros2;
    auto flags = base_flags;
    flags.insert(flags.begin(), std::format("-D{}", macro));
    clang::tooling::FixedCompilationDatabase db(".", flags);
    clang::tooling::ClangTool tool(db, sources);
    EntityExtractionFactory factory(model2, ifdef_macros2);
    tool.run(&factory);

    // Merge: replace/add items from this pass into model.
    // Guarded items take priority over unguarded duplicates.
    for (auto &item : model2.items) {
      bool replaced = false;
      for (auto &existing : model.items) {
        if (existing.kind == item.kind && existing.name == item.name &&
            existing.ns_path == item.ns_path) {
          if (!item.guard_prefix.empty() && existing.guard_prefix.empty()) {
            existing = std::move(item);
          }
          replaced = true;
          break;
        }
      }
      if (!replaced)
        model.items.push_back(std::move(item));
    }
    // Merge macros from pass 2 (may have additional macros in active branches)
    for (auto &m : model2.macros) {
      bool dup = false;
      for (auto &existing : model.macros) {
        if (existing.name == m.name) { dup = true; break; }
      }
      if (!dup)
        model.macros.push_back(std::move(m));
    }
  }

  return model;
}

export EntityModel analyze_files(const std::vector<std::string> &paths) {
  EntityModel combined;
  for (auto &path : paths) {
    auto m = analyze_file(path);
    combined.items.insert_range(combined.items.end(), std::move(m.items));
    combined.macros.insert_range(combined.macros.end(), std::move(m.macros));
  }
  return combined;
}

// NOTE: intentionally a single-pass extraction, unlike analyze_file (which
// re-parses per #ifdef macro). compute_macro_modules depends on this
// single-pass result; making it multi-pass would change macro reachability.
// The per-file entity models, one parse per path. Several analyses want these
// same models — which entities each header declares and completes, which
// macros it defines — and each used to parse the whole header set for itself.
// Callers that need more than one of them should extract once and share.
export std::vector<EntityModel> analyze_files_per_file(
    const std::vector<std::string> &paths,
    const std::vector<std::string> &extra_args) {
  std::vector<EntityModel> partials(paths.size());
  std::vector<std::vector<std::string>> ifdef_macros(paths.size());
  parallel_parse(paths, extra_args, /*delayed_template_parsing=*/false,
                 [&](std::size_t i, const std::string &) {
                   return std::make_unique<EntityExtractionFactory>(
                       partials[i], ifdef_macros[i]);
                 });
  return partials;
}

// The same models flattened into one, in path order.
export EntityModel merge_entity_models(std::vector<EntityModel> partials) {
  EntityModel combined;
  for (auto &model : partials) {
    combined.items.insert_range(combined.items.end(), std::move(model.items));
    combined.macros.insert_range(combined.macros.end(), std::move(model.macros));
  }
  return combined;
}

export EntityModel analyze_files_with_flags(
    const std::vector<std::string> &paths,
    const std::vector<std::string> &extra_args) {
  return merge_entity_models(analyze_files_per_file(paths, extra_args));
}
