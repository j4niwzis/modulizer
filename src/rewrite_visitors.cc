module;

export module modulizer.rewrite_visitors;
import modulizer.astutil;
import modulizer.rewrite_util;
import modulizer.util;
import libtooling;
import std;

// AST visitors that annotate a header/interface unit: export markers, forward-
// declaration handling, used-headers collection, and friend-of-extern-C++
// targets. The header-rewrite consumer (modulizer.rewrite_export) drives them.

export class HeaderVisitor : public NsPathVisitor<HeaderVisitor> {
public:
  HeaderVisitor(clang::SourceManager &sm, std::vector<ModPoint> &mods,
                const std::regex &internal_re,
                std::string_view export_macro,
                std::string_view shared_export_macro,
                std::string_view linkage_macro,
                std::string_view extern_decl_macro,
                const std::vector<std::string> &reachable_fqns,
                std::vector<std::string> &internal_fqns,
                bool no_internal_filter = false,
                const std::vector<std::string> &defined_fqns = {},
                bool extern_cxx = false,
                const std::vector<std::string> &fwd_declared_fqns = {},
                const std::set<std::string> &friend_extern_fqns = {},
                const std::set<std::string> &same_module_free_fqns = {},
                const std::set<std::string> *library_headers = nullptr,
                std::map<std::string, std::vector<ModPoint>> *
                    external_macro_mods = nullptr)
      : sm(sm), mods(mods),
        internal_re(internal_re), export_macro(export_macro),
        shared_export_macro(shared_export_macro),
        linkage_macro(linkage_macro),
        extern_decl_macro(extern_decl_macro),
        reachable_fqns(reachable_fqns), internal_fqns(internal_fqns),
        no_internal_filter(no_internal_filter), defined_fqns(defined_fqns),
        extern_cxx(extern_cxx), fwd_declared_fqns(fwd_declared_fqns),
        friend_extern_fqns(friend_extern_fqns),
        same_module_free_fqns(same_module_free_fqns),
        library_headers(library_headers),
        external_macro_mods(external_macro_mods) {}

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  bool is_internal_ns(const std::string &name) {
    return is_internal(name) || std::regex_match(name, internal_re);
  }

  // Action for a forward declaration (class or function) whose definition
  // lives in another library module:
  //  kNone        — definition in this same file, or the entity is not defined
  //                 by any library header (opaque type): export normally.
  //  kDelete      — the definition is reachable in this TU via an included
  //                 header that becomes an import: the redundant forward
  //                 declaration is removed entirely.
  //  kKeepPrivate — defined by another library module that this file does not
  //                 include: keep the declaration but do not export it.
  enum class FwdAction { kNone, kDelete, kKeepPrivate };
  FwdAction fwd_action(llvm::StringRef name, clang::Decl *def) {
    if (def && isMainFile(def)) return FwdAction::kNone;
    // The definition is visible in this TU through an included library header
    // that becomes an import (e.g. a header fwd-declares `mylib::Foo`, whose
    // definition lives in another header → its own module).
    // The redundant forward declaration must be removed; exporting it would
    // collide with the imported module's entity.
    if (def) return FwdAction::kDelete;
    if (defined_fqns.empty()) return FwdAction::kNone;
    auto fqn = fqn_of(ns_path_, name.str());
    // A declaration whose definition lives in THIS module's implementation
    // unit (a same-stem source file) is a normal module entity: export it and
    // let the impl unit provide the definition. It must not be kept private or
    // made `extern "C++"`, which would make it a distinct global-module entity.
    for (auto &d : same_module_free_fqns) {
      if (matches_reachable(fqn, d)) return FwdAction::kNone;
    }
    for (auto &d : defined_fqns) {
      if (matches_reachable(fqn, d)) return FwdAction::kKeepPrivate;
    }
    return FwdAction::kNone;
  }

  // Apply a forward-declaration action. kDelete removes the declaration
  // (including its trailing newline); kKeepPrivate leaves it as-is (the caller
  // then returns without exporting).
  void apply_fwd_action(clang::Decl *d, FwdAction action) {
    if (action != FwdAction::kDelete) return;
    auto begin = sm.getExpansionLoc(d->getBeginLoc());
    auto end = sm.getExpansionLoc(d->getEndLoc());
    unsigned begin_off = sm.getFileOffset(begin);
    // For a class/function template forward declaration the begin location
    // points at `class`/`struct`, not at the leading `template <...>` header;
    // back up over any preceding template headers so the whole declaration is
    // removed (not just the `class X;` part).
    auto fid = sm.getFileID(begin);
    auto buf = sm.getBufferData(fid);
    if (begin_off > 0) {
      auto p = begin_off;
      while (p > 0 && (buf[p - 1] == ' ' || buf[p - 1] == '\t' ||
                       buf[p - 1] == '\n' || buf[p - 1] == '\r'))
        --p;
      if (p > 0 && buf[p - 1] == '>') {
        auto new_off = template_header_start(buf, p);
        if (new_off != p) begin_off = new_off;
      }
    }
    unsigned semi_off = sm.getFileOffset(end);
    while (semi_off < buf.size() && buf[semi_off] != ';') ++semi_off;
    unsigned del_end = semi_off + 1;
    while (del_end < buf.size() &&
           (buf[del_end] == ' ' || buf[del_end] == '\t' ||
            buf[del_end] == '\r'))
      ++del_end;
    if (del_end < buf.size() && buf[del_end] == '\n') ++del_end;
    mods.push_back({begin_off, del_end - begin_off, ""});
  }

  // A declaration that carries its own `extern` storage-class specifier must
  // not also be given `extern "C++"`: the linkage-specification already
  // supplies the linkage the keyword asks for, and the two together are
  // ill-formed (gcc diagnoses it, clang accepts it):
  //
  //   extern "C++" template <class T> extern T Make(int);
  //   error: invalid use of 'extern' in linkage specification
  //
  // Return the modification that removes the keyword. Dropping it changes
  // nothing: on a function `extern` is what a declaration means anyway, and on
  // a variable it marks a declaration rather than a definition — which
  // `extern "C++"` keeps. The keyword belongs to the declaration itself, whose
  // begin location sits after any template parameter list, so the scan starts
  // there rather than at the marker offset (which precedes `template`).
  std::optional<ModPoint> drop_storage_extern(clang::Decl *d, unsigned off,
                                              llvm::StringRef src,
                                              clang::FileID fid) {
    clang::Decl *inner = d;
    if (auto *td = llvm::dyn_cast<clang::TemplateDecl>(d))
      inner = td->getTemplatedDecl();
    if (!inner) return std::nullopt;
    clang::StorageClass sc = clang::SC_None;
    if (auto *vd = llvm::dyn_cast<clang::VarDecl>(inner))
      sc = vd->getStorageClass();
    else if (auto *fd = llvm::dyn_cast<clang::FunctionDecl>(inner))
      sc = fd->getStorageClass();
    // A declaration inside `extern "C"` has no storage class of its own, so the
    // linkage-specification's own keyword is never the one found here.
    if (sc != clang::SC_Extern) return std::nullopt;
    auto begin = inner->getBeginLoc();
    auto loc = begin.isMacroID() ? sm.getSpellingLoc(begin)
                                 : sm.getExpansionLoc(begin);
    if (!loc.isValid() || sm.getFileID(loc) != fid) return std::nullopt;
    // The marker offset may have been backed up over an attribute macro, which
    // find_extern_spec knows how to skip; never scan from before it.
    unsigned scan = std::max(sm.getFileOffset(loc), off);
    unsigned start = 0, end = 0;
    if (!find_extern_spec(src, scan, start, end)) return std::nullopt;
    // Under the macro spelling the keyword is not removed but replaced: it has
    // to come back wherever the tree is built wrapped, and it has to come back
    // HERE, after any attribute the declaration carries.
    return ModPoint{start, end - start,
                    extern_cxx ? std::format("{} ", extern_decl_macro)
                               : std::string()};
  }

  // Whether an exported variable definition needs `inline` to carry the
  // external linkage an export-declaration requires (see the call site in
  // addExport). Only a definition of a non-volatile const/constexpr variable
  // at namespace scope does: `extern` and `static` ones are already decided,
  // and an array's constness sits on its element type.
  static bool needs_inline_to_export(const clang::VarDecl *vd) {
    if (vd->isInlineSpecified() || vd->isStaticDataMember()) return false;
    if (vd->getStorageClass() != clang::SC_None) return false;
    if (!vd->hasInit()) return false;  // a declaration, not a definition
    auto t = vd->getType();
    while (auto *at = t->getAsArrayTypeUnsafe()) t = at->getElementType();
    if (t.isVolatileQualified()) return false;
    return vd->isConstexpr() || t.isConstQualified();
  }

  // Prefix a forward declaration with `extern "C++"` so it attaches to the
  // global module and merges with the entity's definition in another module.
  // For templates the declaration starts at `template`, not at the struct/class
  // keyword.
  void make_extern_cxx(clang::Decl *d) {
    if (auto *td = d->getDescribedTemplate()) d = td;
    auto begin = d->getBeginLoc();
    // Same macro-body handling as addExport: an entity inside a macro body must
    // get the marker at its spelling location so the macro expansion carries it.
    auto loc = begin.isMacroID() ? sm.getSpellingLoc(begin)
                                 : sm.getExpansionLoc(begin);
    if (!loc.isValid()) return;
    if (!sm.isInMainFile(loc)) loc = sm.getExpansionLoc(begin);
    if (!loc.isValid()) return;
    unsigned off = sm.getFileOffset(loc);
    auto fid = sm.getFileID(loc);
    auto src = sm.getBufferData(fid);
    if (off > 0) {
      // Skip a leading attribute list or attribute macro (e.g. LIB_API_).
      // Attribute macros sit on the same line as the declaration.
      auto p = off - 1;
      while (p > 0 && (src[p] == ' ' || src[p] == '\t')) --p;
      if (p >= 1 && src[p] == ']' && src[p - 1] == ']') {
        while (p > 0 && (src[p] != '[' || src[p - 1] != '[')) --p;
        if (p > 0) off = p - 1;
      } else if (d->hasAttrs() && p > 0 && is_ident_char(src[p])) {
        while (p > 0 && is_ident_char(src[p - 1])) --p;
        off = p;
      }
    }
    // For an out-of-line member-template definition the marker must precede the
    // OUTERMOST `template` header. `getBeginLoc()` of the member's template
    // points at its own header (e.g. `template <typename U>`), but the class
    // template's header (`template <typename T>`) comes first; inserting
    // between the two headers is ill-formed. Scan backward over any number of
    // preceding `template <...>` headers.
    if (off > 0) {
      // `pos` points just past a `>` that closes a template parameter list.
      // Find the matching `<`, then check that `template` precedes it.
      while (off > 0) {
        // Move left past whitespace to the character before the marker.
        auto p = off;
        while (p > 0 && (src[p - 1] == ' ' || src[p - 1] == '\t' ||
                         src[p - 1] == '\n' || src[p - 1] == '\r'))
          --p;
        if (p == 0 || src[p - 1] != '>') break;
        auto new_off = template_header_start(src, p);
        if (new_off == p) break;
        off = new_off;
      }
    }
    if (auto m = drop_storage_extern(d, off, src, fid))
      mods.push_back(std::move(*m));
    mods.push_back({off, 0, extern_cxx ? std::format("{} ", linkage_macro)
                                       : std::string("extern \"C++\" ")});
  }

  // Remove the `static` storage-class keyword from a function definition so it
  // gains external linkage (used for consumer-reachable static function
  // templates that must be re-homed in the global module).
  void strip_static(clang::FunctionDecl *fd) {
    auto begin = sm.getExpansionLoc(fd->getBeginLoc());
    if (!begin.isValid()) return;
    auto fid = sm.getFileID(begin);
    auto src = sm.getBufferData(fid);
    unsigned off = sm.getFileOffset(begin);
    // Scan forward for the `static` keyword before the function body.
    auto text = std::string_view(src.data() + off, src.size() - off);
    auto brace = text.find('{');
    if (brace == std::string_view::npos) brace = text.size();
    auto prefix = text.substr(0, brace);
    auto kw = prefix.find("static");
    while (kw != std::string_view::npos) {
      // Confirm a whole token: delimiter before, whitespace after.
      bool boundary_before = kw == 0 || !is_ident_char(prefix[kw - 1]);
      bool boundary_after = kw + 6 < prefix.size() &&
          (prefix[kw + 6] == ' ' || prefix[kw + 6] == '\t' ||
           prefix[kw + 6] == '\n' || prefix[kw + 6] == '\r');
      if (boundary_before && boundary_after) break;
      kw = prefix.find("static", kw + 6);
    }
    if (kw == std::string_view::npos) return;
    unsigned start = off + static_cast<unsigned>(kw);
    unsigned end = start + 6;
    while (end < src.size() &&
           (src[end] == ' ' || src[end] == '\t' || src[end] == '\n'))
      ++end;
    mods.push_back({start, end - start, ""});
  }

  // True when this module declares `name` (forward-declared) but does not
  // define it; the definition lives in another module, so both this module's
  // declaration and that definition need `extern "C++"`.
  bool cross_module_fwd_declared(llvm::StringRef name) {
    if (fwd_declared_fqns.empty()) return false;
    auto fqn = fqn_of(ns_path_, name.str());
    for (auto &d : fwd_declared_fqns) {
      if (matches_reachable(fqn, d)) return true;
    }
    return false;
  }

  // True when this entity must be `extern "C++"`: either it is declared in a
  // module that does not define it, or it is friend-declared inside an
  // extern "C++" class (which attaches the declaration to the global module).
  bool is_extern_cxx_entity(llvm::StringRef name) {
    if (cross_module_fwd_declared(name)) return true;
    if (friend_extern_fqns.empty()) return false;
    auto fqn = fqn_of(ns_path_, name.str());
    for (auto &f : friend_extern_fqns) {
      if (matches_reachable(fqn, f)) return true;
    }
    return false;
  }

  bool VisitCXXRecordDecl(clang::CXXRecordDecl *rd) {
    if (!rd || rd->isImplicit() || !isMainFile(rd)) return true;
    if (llvm::isa<clang::CXXRecordDecl>(rd->getDeclContext())) return true;
    if (llvm::isa<clang::FunctionDecl>(rd->getDeclContext())) return true;
    if (rd->getFriendObjectKind() != clang::Decl::FOK_None) return true;
    // A friend class TEMPLATE (`template <typename T> friend class X;`) marks
    // its described ClassTemplateDecl as the friend, not the inner record; the
    // record's own friend kind is FOK_None. Such a declaration inside a class
    // body must never be exported or wrapped.
    if (auto *ct = rd->getDescribedClassTemplate())
      if (ct->getFriendObjectKind() != clang::Decl::FOK_None) return true;
    if (!rd->isCompleteDefinition()) {
      // A primary class template forward declaration whose partial
      // specialization is defined in THIS file (e.g. `template <typename T>
      // struct Function;` + `template <typename R, typename...> struct
      // Function<R(Args...)>`). The primary itself is never "complete", but
      // this module owns the specialization, so the primary must be exported
      // for consumers to name `Function<X>`. Without this check, defined_fqns
      // (which includes the specialization's FQN) would classify the primary
      // as defined-elsewhere and keep it private.
      auto *ct = rd->getDescribedClassTemplate();
      if (ct) {
        bool spec_in_file = false;
        llvm::SmallVector<clang::ClassTemplatePartialSpecializationDecl *, 4>
            partial_specs;
        ct->getPartialSpecializations(partial_specs);
        for (auto *spec : partial_specs) {
          if (spec->isCompleteDefinition() && isMainFile(spec)) {
            spec_in_file = true;
            break;
          }
        }
        if (spec_in_file) {
          bool need_extern = is_extern_cxx_entity(rd->getNameAsString());
          addExport(rd, need_extern);
          return true;
        }
      }
      // Forward declaration of a class fully defined by another library module.
      auto action = fwd_action(rd->getNameAsString(), rd->getDefinition());
      apply_fwd_action(rd, action);
      if (action == FwdAction::kDelete) return true;
      if (action == FwdAction::kKeepPrivate) {
        // The defining module is not imported here, so this declaration has to
        // be a global-module entity that merges with the definition. Being in
        // the global module is not enough to make it ONE entity, though: a
        // module that cannot see this declaration introduces its own and the
        // two never merge, so every use across the boundary mismatches
        // ("ambiguating new declaration"). The export is what lets importers
        // find and merge with it.
        //
        // The same either way. The wrapping decides how the marker is SPELLED —
        // it expands to `extern "C++"` where the wrapping is absent and to
        // nothing where the enclosing block already supplies it — not whether
        // the declaration is exported. Leaving it unexported without the
        // wrapping made the two builds disagree about what a module offers, and
        // an importer that was told the entity is exported found nothing:
        //
        //   error: declaration of 'X' must be imported from module 'lib.other'
        //          before it is required
        //   note: declaration here is not visible
        addExport(rd, /*need_extern_cxx=*/false, /*shared=*/true);
        return true;
      }
    }
    bool need_extern = is_extern_cxx_entity(rd->getNameAsString());
    if (no_internal_filter) { addExport(rd, need_extern); return true; }
    if (is_internal(rd->getNameAsString())) {
      // A forward-declared internal entity that the consumer defines itself
      // (e.g. a library's `ListenerAccessor`) must be declared
      // `extern "C++"` so it lands in the global module: the consumer's
      // definition then merges with it and the module's friend declarations
      // keep granting access to the private members. A complete internal
      // definition stays a module entity.
      //
      // Unless the definition is in THIS file. Then there is no consumer to
      // merge with and nothing to reach across: the entity belongs to this
      // module, and putting only its earlier declaration in the global module
      // splits one entity into two —
      //
      //   error: declaration of 'X' in module lib.thing follows declaration
      //          in the global module
      //
      // Declaring a template ahead of its definition so an alias in between
      // can name it is ordinary; it is not a cross-module signal.
      bool defined_here =
          rd->getDefinition() && isMainFile(rd->getDefinition());
      if (!rd->isCompleteDefinition() && !extern_cxx && !defined_here) {
        make_extern_cxx(rd);
        return true;
      }
      // An internal-namespace class whose members are defined in another
      // module is `extern "C++"`; it must also be exported so the defining
      // impl unit can see its declaration. The export is owed whenever the
      // entity is one of those, not only when this rewrite is the thing adding
      // the marker — under the whole-body wrapping the block supplies the
      // linkage and the export would otherwise be dropped.
      if (is_extern_cxx_entity(rd->getNameAsString()))
        addExport(rd, need_extern);
      return true;
    }
    addExport(rd, need_extern);
    return true;
  }

  bool VisitFunctionDecl(clang::FunctionDecl *fd) {
    if (!fd || fd->isImplicit() || !isMainFile(fd)) return true;
    if (llvm::isa<clang::CXXMethodDecl>(fd)) {
      // Out-of-line member definition in this header: when the class is
      // `extern "C++"`, the definition must be too, or it would be a distinct
      // module entity conflicting with the class's global-module declaration.
      auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(fd);
      if (!md->isThisDeclarationADefinition()) return true;
      if (llvm::isa<clang::CXXRecordDecl>(md->getLexicalDeclContext()))
        return true;  // in-class definition, covered by the class
      auto *cls = llvm::dyn_cast<clang::CXXRecordDecl>(md->getDeclContext());
      if (!cls) return true;
      if (is_extern_cxx_entity(cls->getNameAsString()))
        make_extern_cxx(fd);
      return true;
    }
    if (fd->getFriendObjectKind() != clang::Decl::FOK_None) return true;
    // A namespace-scope `static` function has internal linkage and can never be
    // exported or given `extern "C++"`. When consumers (impl units) use it,
    // however, it must be re-homed in the global module: strip the `static`
    // and export it so impls importing the module can use it. (In the interface
    // unit the definition exists exactly once, so there is no ODR risk.)
    if (fd->getStorageClass() == clang::SC_Static) {
      bool reachable = false;
      auto fqn = fqn_of(ns_path_, fd->getNameAsString());
      for (auto &r : reachable_fqns) {
        if (matches_reachable(fqn, r)) { reachable = true; break; }
      }
      if (!reachable) return true;
      strip_static(fd);
      addExport(fd);
      return true;
    }
    if (!fd->isThisDeclarationADefinition()) {
      // Forward declaration of a function defined by another library module.
      auto action = fwd_action(fd->getNameAsString(), fd->getDefinition());
      apply_fwd_action(fd, action);
      if (action == FwdAction::kDelete) return true;
      if (action == FwdAction::kKeepPrivate) {
        // The defining module is not imported here: keep the declaration as a
        // global-module entity so it merges with the definition, and export it
        // or a module that cannot see it introduces its own entity of the same
        // name. The same either way, as for records above — the marker's
        // linkage macro supplies the `extern "C++"` where the wrapping is
        // absent, so only the spelling differs.
        addExport(fd, /*need_extern_cxx=*/false, /*shared=*/true);
        return true;
      }
    }
    bool need_extern = is_extern_cxx_entity(fd->getNameAsString());
    if (is_internal(fd->getNameAsString())) {
      // An internal-namespace function declared in this header but defined in
      // another module is `extern "C++"`; it must also be exported so the
      // defining impl unit can see its declaration. As with records, the export
      // follows from what the entity is, not from whether this rewrite is the
      // thing adding the marker.
      if (is_extern_cxx_entity(fd->getNameAsString()))
        addExport(fd, need_extern);
      return true;
    }
    addExport(fd, need_extern);
    return true;
  }

  bool VisitEnumDecl(clang::EnumDecl *ed) {
    if (!ed || ed->isImplicit() || !isMainFile(ed)) return true;
    if (llvm::isa<clang::CXXRecordDecl>(ed->getDeclContext())) return true;
    if (no_internal_filter) { addExport(ed); return true; }
    if (is_internal(ed->getNameAsString())) {
      // An internal enum whose *value* is reachable (e.g. `kFatal` named
      // by a public macro) must be exported so impl units and consumers can
      // name the enumerator. Enumerators are declared in the enum's enclosing
      // namespace, so a reachable FQN like `internal::kFatal` can never
      // match the enum decl by name; fall back to matching each enumerator.
      bool reachable_value = false;
      for (auto *en : ed->enumerators()) {
        auto fqn = fqn_of(ns_path_, en->getNameAsString());
        for (auto &r : reachable_fqns) {
          if (r == fqn || fqn.ends_with("::" + r)) {
            reachable_value = true;
            break;
          }        }
        if (reachable_value) break;
      }
      if (!reachable_value) return true;
    }
    addExport(ed);
    return true;
  }

  bool VisitTypedefDecl(clang::TypedefDecl *td) {
    if (!td || td->isImplicit() || !isMainFile(td)) return true;
    if (llvm::isa<clang::CXXRecordDecl>(td->getDeclContext())) return true;
    if (llvm::isa<clang::FunctionDecl>(td->getDeclContext())) return true;
    if (is_internal(td->getNameAsString())) return true;
    addExport(td);
    return true;
  }

  bool VisitTypeAliasDecl(clang::TypeAliasDecl *ta) {
    if (!ta || ta->isImplicit() || !isMainFile(ta)) return true;
    if (llvm::isa<clang::CXXRecordDecl>(ta->getDeclContext())) return true;
    if (llvm::isa<clang::FunctionDecl>(ta->getDeclContext())) return true;
    if (is_internal(ta->getNameAsString())) return true;
    addExport(ta);
    return true;
  }

  bool VisitTypeAliasTemplateDecl(clang::TypeAliasTemplateDecl *tat) {
    if (!tat || tat->isImplicit() || !isMainFile(tat)) return true;
    if (auto *ta = tat->getTemplatedDecl()) {
      if (llvm::isa<clang::CXXRecordDecl>(ta->getDeclContext())) return true;
      if (llvm::isa<clang::FunctionDecl>(ta->getDeclContext())) return true;
    }
    // A cross-module alias template (e.g. `IsConvertible`
    // referenced inside another module's template bodies) must be `extern
    // "C++"` on BOTH sides so the injected copy in the using module and this
    // definition stay one shared entity.
    if (is_extern_cxx_entity(tat->getNameAsString())) {
      make_extern_cxx(tat);
      return true;
    }
    if (is_internal(tat->getNameAsString())) return true;
    addExport(tat);
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *vd) {
    if (!vd || vd->isImplicit() || !isMainFile(vd)) return true;
    if (vd->isLocalVarDeclOrParm() || vd->isStaticDataMember()) return true;
    if (vd->isStaticLocal()) return true;
    if (is_internal(vd->getNameAsString())) return true;
    // A variable declared in this header but defined in another module must be
    // `extern "C++"` on both sides to stay a single shared entity.
    bool need_extern = is_extern_cxx_entity(vd->getNameAsString());
    addExport(vd, need_extern);
    return true;
  }

  bool VisitUsingDecl(clang::UsingDecl *ud) {
    // A namespace-scope `using` declaration (e.g. `using internal::Widget;`)
    // hoists a name for consumers. It is a reachable entity and must be
    // exported so the module exposes the name.
    if (!ud || ud->isImplicit() || !isMainFile(ud)) return true;
    if (llvm::isa<clang::CXXRecordDecl>(ud->getDeclContext())) return true;
    if (llvm::isa<clang::FunctionDecl>(ud->getDeclContext())) return true;
    addExport(ud);
    return true;
  }

  bool VisitUsingDirectiveDecl(clang::UsingDirectiveDecl *udd) {
    // A namespace-scope `using namespace X;` directive (e.g. a library's
    // `using namespace detail;` inside `namespace lib`) makes X's members
    // visible in the enclosing namespace, and consumers resolved names like
    // `lib::Foo` (which lives in `lib::detail`) through it.
    //
    // Exporting the directive does not carry that across a module boundary: a
    // using-directive declares no name, so there is nothing for the export to
    // apply to, and an importer's lookup never sees the members —
    //
    //   error: 'Foo' is not a member of 'lib'
    //
    // (one implementation honours it anyway, which is why this went unnoticed).
    // A using-DECLARATION does declare a name and does cross the boundary, so
    // the directive stays for the library's own unqualified lookups and one
    // exported declaration per name is emitted after it.
    if (!udd || udd->isImplicit() || !isMainFile(udd)) return true;
    if (llvm::isa<clang::CXXRecordDecl>(udd->getDeclContext())) return true;
    if (llvm::isa<clang::FunctionDecl>(udd->getDeclContext())) return true;
    auto *ns = udd->getNominatedNamespace();
    // Only a namespace this header itself declares: `using namespace std;` is
    // not this rewrite's to re-declare.
    if (!ns || !isMainFile(ns)) { addExport(udd); return true; }
    std::set<std::string> names;
    for (auto *d : ns->decls()) {
      auto *nd = llvm::dyn_cast<clang::NamedDecl>(d);
      if (!nd || nd->isImplicit()) continue;
      if (llvm::isa<clang::UsingDirectiveDecl>(nd) ||
          llvm::isa<clang::NamespaceDecl>(nd))
        continue;
      auto name = nd->getDeclName();
      if (!name.isIdentifier() || nd->getName().empty()) continue;
      // One declaration names every overload, so the set does the deduping.
      names.insert(nd->getName().str());
    }
    if (names.empty()) return true;
    auto end = sm.getExpansionLoc(udd->getEndLoc());
    if (!end.isValid()) return true;
    auto fid = sm.getFileID(end);
    auto buf = sm.getBufferData(fid);
    unsigned off = sm.getFileOffset(end);
    while (off < buf.size() && buf[off] != ';') ++off;
    if (off >= buf.size()) return true;
    ++off;  // just past the `;`
    std::string text;
    for (auto &n : names)
      text += std::format("\n{} using {}::{};", export_macro,
                          ns->getNameAsString(), n);
    mods.push_back({off, 0, std::move(text)});
    return true;
  }

private:
  const clang::SourceManager &sm;
  std::vector<ModPoint> &mods;
  const std::regex &internal_re;
  std::string_view export_macro;
  std::string_view shared_export_macro;
  // How `extern "C++"` is spelled. A tree generated with the wrapping defers
  // the choice to build time: the macro is nothing when the purview is wrapped
  // (the block supplies the linkage) and `extern "C++"` when it is not.
  std::string_view linkage_macro;
  // The spelling for a declaration whose own `extern` the marker replaces.
  std::string_view extern_decl_macro;
  const std::vector<std::string> &reachable_fqns;
  bool no_internal_filter = false;
  std::vector<std::string> &internal_fqns;
  const std::vector<std::string> &defined_fqns;
  bool extern_cxx = false;
  const std::vector<std::string> &fwd_declared_fqns;
  const std::set<std::string> &friend_extern_fqns;
  const std::set<std::string> &same_module_free_fqns;
  // Library headers being rewritten in this run (absolute paths). When an
  // entity is defined inside a macro body whose definition lives in one of
  // these OTHER headers (e.g. a macro defined in matchers.h
  // but invoked in more-matchers.h), the export marker must be routed
  // into that header's macros file instead of exported at the invocation.
  const std::set<std::string> *library_headers = nullptr;
  std::map<std::string, std::vector<ModPoint>> *external_macro_mods = nullptr;

  bool is_internal(const std::string &entity_name) const {
    if (no_internal_filter) return false;
    if (internal_depth_ == 0) return false;
    auto fqn = fqn_of(ns_path_, entity_name);
    for (auto &r : reachable_fqns) {
      if (matches_reachable(fqn, r)) return false;
    }
    internal_fqns.push_back(fqn);
    return true;
  }

  bool isMainFile(clang::Decl *d) const {
    if (!d) return false;
    auto loc = d->getLocation();
    return loc.isValid() && sm.isInMainFile(loc);
  }

  // `shared`: this declares an entity another module defines, so the marker is
  // the one whose expansion depends on whether the purview is wrapped.
  void addExport(clang::Decl *d, bool need_extern_cxx = false,
                 bool shared = false) {
    // [module.interface]/3: an export-declaration shall not declare a partial
    // or explicit specialization. A specialization is exported together with
    // its primary template, so the marker is simply omitted. (clang accepts
    // the marker; gcc rejects it, and the standard is on gcc's side.)
    // The `extern "C++"` is still required: a specialization of a template
    // attached to the global module must be attached to it as well, or it
    // specializes a different entity.
    const bool specialization = is_specialization(d);
    if (specialization && !need_extern_cxx) return;
    if (auto *rd = llvm::dyn_cast<clang::CXXRecordDecl>(d)) {
      if (rd->getFriendObjectKind() != clang::Decl::FOK_None) return;
      if (auto *ct = rd->getDescribedClassTemplate())
        if (ct->getFriendObjectKind() != clang::Decl::FOK_None) return;
      if (llvm::isa<clang::CXXRecordDecl>(rd->getLexicalDeclContext())) return;
    }
    if (auto *fd = llvm::dyn_cast<clang::FunctionDecl>(d)) {
      if (fd->getFriendObjectKind() != clang::Decl::FOK_None) return;
      if (llvm::isa<clang::CXXRecordDecl>(fd->getLexicalDeclContext())) return;
    }
    if (auto *td = d->getDescribedTemplate()) d = td;
    auto begin = d->getBeginLoc();
    // An entity defined inside a macro body (`#define MACRO(x) ... class Foo
    // { x };`) has a begin location whose expansion point is the macro
    // invocation. Insert the marker at the macro-body spelling location so the
    // class itself carries the export wherever the macro expands, instead of
    // exporting the whole invocation.
    auto loc = begin.isMacroID() ? sm.getSpellingLoc(begin)
                                 : sm.getExpansionLoc(begin);
    if (!loc.isValid()) return;
    // When the macro body lives in ANOTHER library header being rewritten
    // (e.g. a macro defined in matchers.h but invoked in
    // more-matchers.h), the marker must be routed into that header's
    // macros file (so the macro body bakes the export in wherever it expands),
    // not exported at the invocation in this file. Variables are exempt: a
    // shared DECLARE macro (e.g. `DECLARE_FLAG_`) expands to a
    // single `extern` variable whose linkage differs per use (module-local in
    // some modules, `extern "C++"` cross-module in others); baking the marker
    // into the shared body would force one linkage on every expansion. Such
    // entities keep the invocation-level export.
    std::string route_to;
    if (!sm.isInMainFile(loc) && external_macro_mods && library_headers &&
        !llvm::isa<clang::VarDecl>(d)) {
      auto fn = sm.getFilename(loc);
      if (!fn.empty()) {
        auto canon =
            std::filesystem::weakly_canonical(std::filesystem::path(fn.str()))
                .string();
        if (library_headers->count(canon)) route_to = canon;
      }
    }
    if (route_to.empty() && !sm.isInMainFile(loc)) loc = sm.getExpansionLoc(begin);
    if (!loc.isValid()) return;
    unsigned off = sm.getFileOffset(loc);
    // Move before leading [[]] attributes which getBeginLoc skips
    auto fid = sm.getFileID(loc);
    auto src = sm.getBufferData(fid);
    if (off > 0) {
      auto p = off - 1;
      while (p > 0 && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n'))
        --p;
      if (p >= 1 && src[p] == ']' && src[p - 1] == ']') {
        while (p > 0 && (src[p] != '[' || src[p - 1] != '[')) --p;
        if (p > 0) off = p - 1;
      } else if (d->hasAttrs()) {
        // A leading attribute *macro* (e.g. LIB_API_ → `[[gnu::visibility]]`)
        // makes getBeginLoc point past it. Place the export before the macro so
        // the attribute list doesn't precede `export extern "C++"`. Attribute
        // macros sit on the same line as the declaration, so only skip spaces
        // and tabs (never a newline, which could be a `#if` line above).
        auto q = off;
        while (q > 0 && (src[q - 1] == ' ' || src[q - 1] == '\t'))
          --q;
        if (q > 0 && is_ident_char(src[q - 1])) {
          while (q > 0 && is_ident_char(src[q - 1])) --q;
          off = q;
        }
      }
    }
    // A declaration inside an unbraced linkage-specification (`extern "C" void
    // f();`) begins after the `extern "C"`, so a marker placed there produces
    // `extern "C" export void f();`. The export-declaration must come first
    // ([dcl.link] admits `export extern "C" …`, not the reverse), so back the
    // offset up to the linkage-specification itself.
    if (auto *ls = llvm::dyn_cast_or_null<clang::LinkageSpecDecl>(
            d->getLexicalDeclContext()))
      if (!ls->hasBraces()) {
        auto ls_loc = sm.getExpansionLoc(ls->getBeginLoc());
        if (ls_loc.isValid() && sm.getFileID(ls_loc) == fid)
          off = sm.getFileOffset(ls_loc);
      }
    std::string prefix =
        specialization
            ? std::string()
            : std::format("{} ", shared ? shared_export_macro : export_macro);
    if (need_extern_cxx) {
      // `extern int x;` and `template <class T> extern T f();` carry the
      // storage-class specifier that `extern "C++"` makes redundant — and
      // ill-formed next to it. Drop it so the result is a clean single
      // `extern "C++" int x;`.
      if (auto m = drop_storage_extern(d, off, src, fid)) {
        if (!route_to.empty())
          (*external_macro_mods)[route_to].push_back(std::move(*m));
        else
          mods.push_back(std::move(*m));
      }
      prefix += extern_cxx ? std::format("{} ", linkage_macro)
                           : std::string("extern \"C++\" ");
    }
    // A namespace-scope `const`/`constexpr` variable has internal linkage, and
    // an export-declaration must declare a name with external linkage. In a
    // module purview the export itself grants it ([basic.link]/3.2 exempts an
    // exported variable), but under the `extern "C++"` wrapping the entity is
    // attached to the global module, where that exemption does not apply:
    //
    //   extern "C++" { export constexpr int k = 5; }
    //   error: exporting declaration 'k' with internal linkage
    //
    // `inline` grants external linkage either way, and one shared object is
    // what an exported constant means in the first place. The header cannot
    // know which way the wrapping macro will be set at build time, so this is
    // unconditional rather than tied to the wrapping.
    if (!specialization)
      if (auto *vd = llvm::dyn_cast<clang::VarDecl>(d))
        if (needs_inline_to_export(vd)) prefix += "inline ";
    ModPoint m{off, 0, std::move(prefix)};
    if (!route_to.empty())
      (*external_macro_mods)[route_to].push_back(std::move(m));
    else
      mods.push_back(std::move(m));
  }
};

// Collects the resolved files of declarations referenced by the main file.
// Template instantiations are visited so usages inside template bodies are
// caught; this runs separately from the export visitor so instantiated decls
// never receive duplicate export annotations.
export class UsedHeadersVisitor : public clang::RecursiveASTVisitor<UsedHeadersVisitor> {
public:
  UsedHeadersVisitor(const clang::SourceManager &sm, std::set<std::string> &used)
      : sm(sm), used(used) {}

  bool shouldVisitTemplateInstantiations() const { return true; }
  bool shouldVisitImplicitCode() const { return false; }

private:
  void record(clang::Decl *d) {
    if (!d) return;
    auto loc = d->getLocation();
    if (!loc.isValid()) return;
    auto f = sm.getFilename(loc);
    if (f.empty() || sm.isInMainFile(loc)) return;
    used.insert(f.str());
    // Also record the include chain: a used declaration may live in a header
    // that a forwarding header pulls in via `#include_next` (libc++
    // `<cstdio>`/`<stdio.h>` forward to musl's `<stdio.h>`). The GMF keeps the
    // header the source textually included, so every header between the decl
    // and the main file is marked used.
    auto fid = sm.getFileID(loc);
    unsigned guard = 0;
    while (++guard < 64) {
      auto inc_loc = sm.getIncludeLoc(fid);
      if (!inc_loc.isValid()) break;
      auto inc_f = sm.getFilename(inc_loc);
      if (inc_f.empty() || sm.isInMainFile(inc_loc)) break;
      used.insert(inc_f.str());
      fid = sm.getFileID(inc_loc);
    }
  }

  bool in_main(clang::SourceLocation loc) const {
    return loc.isValid() && sm.isInMainFile(loc);
  }

public:
  bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
    if (e && in_main(e->getLocation())) record(e->getDecl());
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *e) {
    if (e && in_main(e->getMemberLoc())) record(e->getMemberDecl());
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *e) {
    if (e && in_main(e->getExprLoc())) record(e->getCalleeDecl());
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *e) {
    if (e && in_main(e->getExprLoc())) record(e->getConstructor());
    return true;
  }

  bool VisitTypeLoc(clang::TypeLoc tl) {
    if (!tl || !in_main(tl.getBeginLoc())) return true;
    auto *t = tl.getTypePtr();
    if (!t) return true;
    if (auto *td = t->getAsTagDecl()) { record(td); return true; }
    if (auto *tst = llvm::dyn_cast<clang::TemplateSpecializationType>(t)) {
      if (auto *td = tst->getTemplateName().getAsTemplateDecl()) record(td);
    }
    return true;
  }

private:
  const clang::SourceManager &sm;
  std::set<std::string> &used;
};

// Collects friend-declaration targets of extern "C++" classes. A friend
// declaration inside an extern "C++" class attaches to the global module, so
// the friend target's own definition must also be `extern "C++"` (even when it
// lives in the same module) or it would be a distinct module entity that
// conflicts with the friend's global-module declaration.
export class FriendExternCxxCollector
    : public clang::RecursiveASTVisitor<FriendExternCxxCollector> {
public:
  FriendExternCxxCollector(const clang::SourceManager &sm,
                           const std::set<std::string> &fwd_declared_fqns,
                           std::set<std::string> &out)
      : sm(sm), fwd_declared_fqns(fwd_declared_fqns), out(out) {}

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldVisitImplicitCode() const { return false; }

  bool VisitFriendDecl(clang::FriendDecl *f) {
    if (!f) return true;
    auto *cls =
        llvm::dyn_cast_or_null<clang::CXXRecordDecl>(f->getDeclContext());
    if (!cls) return true;
    auto loc = cls->getLocation();
    if (!loc.isValid() || !sm.isInMainFile(loc)) return true;
    if (!fwd_declared_fqns.count(cls->getQualifiedNameAsString())) return true;
    if (auto *tsi = f->getFriendType())
      if (auto *td = tsi->getType()->getAsTagDecl())
        out.insert(td->getQualifiedNameAsString());
    if (auto *nd = llvm::dyn_cast_or_null<clang::NamedDecl>(f->getFriendDecl()))
      out.insert(nd->getQualifiedNameAsString());
    return true;
  }

private:
  const clang::SourceManager &sm;
  const std::set<std::string> &fwd_declared_fqns;
  std::set<std::string> &out;
};
