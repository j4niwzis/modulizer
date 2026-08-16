module;

export module modulizer.wrapper_gen;
import modulizer.analyzer;
import modulizer.util;
import libtooling;
import std;

namespace {

bool is_internal(const std::string &seg, const std::regex &re) {
  return std::regex_match(seg, re);
}

bool has_internal_path(const std::vector<std::string> &ns_path,
                       const std::regex &re) {
  for (auto &seg : ns_path) {
    if (is_internal(seg, re)) return true;
  }
  return false;
}

// A re-exportable entity name is a plain identifier (or operator name). Names
// that carry template-argument syntax — dependent names like `Foo<T>` or the
// corrupted `operator Foo<type-parameter-1-0>` — are not real entities: the
// primary template is already exported by default, and re-exporting a
// specialization by name would emit an invalid `using` declaration.
bool is_dependent_name(const std::string &name) {
  if (name.contains("type-parameter")) return true;
  if (name.contains(' ')) return true;
  auto lt = name.find('<');
  if (lt != std::string::npos && !name.starts_with("operator")) return true;
  return false;
}

struct NsNode {
  std::vector<const EntityItem *> items;
  std::map<std::string, NsNode, std::less<>> children;
};

// Whether the wrapper carries this entity across at all: public entities
// always, internal ones only when a consumer or a macro reaches them. Says
// nothing about HOW it is carried — a using-declaration or, for the ones no
// using-declaration can name, a copy of the value.
bool wrapper_visible(const EntityItem &item, const std::regex &re,
                     const std::set<std::string> &reachable) {
  if (is_dependent_name(item.name)) return false;
  // Never re-export into namespace std. A header may declare things there (an
  // ADL `swap`, a `hash` specialisation); re-opening std to re-export them is
  // undefined behaviour, and they are not part of the wrapped library's API in
  // any case.
  if (!item.ns_path.empty() && item.ns_path.front() == "std") return false;
  if (!has_internal_path(item.ns_path, re)) return true;
  auto fqn = fqn_of(item.ns_path, item.name);
  for (auto &r : reachable) {
    if (r == fqn || fqn.ends_with("::" + r)) return true;
  }
  return false;
}

void insert(NsNode &root, const EntityItem &item, std::size_t depth = 0) {
  if (depth >= item.ns_path.size()) {
    root.items.push_back(&item);
    return;
  }
  insert(root.children[item.ns_path[depth]], item, depth + 1);
}

// The entities a using-declaration cannot name but whose value can be copied.
std::vector<const EntityItem *> collect_constants(
    const EntityModel &model, llvm::StringRef internal_filter,
    const std::vector<std::string> &reachable_internal) {
  std::regex re(internal_filter.str());
  std::set<std::string> reachable(reachable_internal.begin(),
                                   reachable_internal.end());
  std::vector<const EntityItem *> out;
  for (const auto &item : model.items) {
    if (!item.no_external_linkage || !item.constant_value) continue;
    if (!wrapper_visible(item, re, reachable)) continue;
    out.push_back(&item);
  }
  return out;
}

// Emit `inline constexpr auto <name> = <source>;` for each constant, grouped
// by the namespace it belongs to. `ns_prefix` nests the whole tree under a
// namespace of its own; `value_prefix` is where the value is read FROM. One
// side sets one of them and the other side sets the other: the constants
// module declares prefixed names reading the header's, and the facade declares
// the real names reading the prefixed ones.
void emit_constants(llvm::raw_ostream &os,
                    const std::vector<const EntityItem *> &items,
                    llvm::StringRef ns_prefix, llvm::StringRef value_prefix) {
  using GuardKey = std::pair<std::string, std::string>;
  std::map<std::string, std::map<GuardKey, std::set<std::string>>> grouped;
  for (auto *it : items) {
    std::string path;
    for (auto &seg : it->ns_path) {
      if (!path.empty()) path += "::";
      path += seg;
    }
    grouped[path][{it->guard_prefix, it->guard_suffix}].insert(it->name);
  }

  for (const auto &[path, by_guard] : grouped) {
    std::string open_ns = ns_prefix.str();
    if (!path.empty()) {
      if (!open_ns.empty()) open_ns += "::";
      open_ns += path;
    }
    if (!open_ns.empty()) os << "export namespace " << open_ns << " {\n";
    for (const auto &[guard, names] : by_guard) {
      if (!guard.first.empty()) os << guard.first;
      for (const auto &n : names) {
        std::string src = "::";
        if (!value_prefix.empty()) src += value_prefix.str() + "::";
        if (!path.empty()) src += path + "::";
        src += n;
        os << (open_ns.empty() ? "export " : "  ") << "inline constexpr auto "
           << n << " = " << src << ";\n";
      }
      if (!guard.second.empty()) os << guard.second;
    }
    if (!open_ns.empty()) os << "}\n";
  }
}

void emit_items(llvm::raw_ostream &os, llvm::StringRef indent,
                const std::vector<const EntityItem *> &items,
                bool export_using,
                const std::set<std::string> &extern_cxx) {
  using GuardKey = std::pair<std::string, std::string>;
  std::map<GuardKey, std::map<std::string, EntityItem::Kind, std::less<>>> grouped;
  for (auto *it : items) {
    GuardKey gk = {it->guard_prefix, it->guard_suffix};
    std::string path;
    for (auto &seg : it->ns_path) {
      if (!path.empty()) path += "::";
      path += seg;
    }
    if (!path.empty()) path += "::";
    path += it->name;
    grouped[gk][path] = it->kind;
  }
  for (const auto &[guard, decls] : grouped) {
    if (!guard.first.empty()) os << indent << guard.first;
    for (const auto &[fqn, kind] : decls) {
      os << indent;
      if (!guard.first.empty()) os << "  ";
      if (export_using) os << "export ";
      if (kind == EntityItem::kUsingDirective) {
        // A using-namespace directive re-exports the nominated namespace's
        // members relative to the enclosing namespace (e.g. `adl_guard` inside
        // `namespace mylib`). Re-emit the directive itself so consumers
        // resolve those names through it.
        auto pos = fqn.rfind("::");
        os << "using namespace "
           << (pos == std::string::npos ? fqn : fqn.substr(pos + 2)) << ";\n";
      } else if (extern_cxx.count(fqn)) {
        // A cross-module friend-declared entity (e.g.
        // `mylib::internal::Factory`, friend-declared in the
        // umbrella's interface class) lives in the global module as an
        // `extern "C++"` shared entity. Its re-export must carry the same
        // linkage so it binds to the definition in the defining module.
        os << "extern \"C++\" using ::" << fqn << ";\n";
      } else {
        os << "using ::" << fqn << ";\n";
      }
    }
    if (!guard.second.empty()) os << indent << guard.second;
  }
}

void emit_tree(llvm::raw_ostream &os, const NsNode &node,
               const std::string &prefix, int depth, bool export_ns,
               const std::set<std::string> &extern_cxx) {
  std::string indent(depth * 2, ' ');
  if (!node.items.empty()) {
    // Global-namespace entities have no namespace block to export them, so
    // their using-declarations carry `export` themselves — unless the caller
    // already wrapped the whole body in an exported linkage block, in which
    // case a nested `export` would be redundant (and ill-formed).
    emit_items(os, indent, node.items, depth == 0 && export_ns, extern_cxx);
  }
  for (const auto &[name, child] : node.children) {
    if (child.items.empty() && child.children.empty()) continue;
    std::string full = prefix.empty() ? name : prefix + "::" + name;
    if (export_ns) {
      os << indent << "export namespace " << name << " {\n";
    } else {
      os << indent << "namespace " << name << " {\n";
    }
    emit_tree(os, child, full, depth + 1, false, extern_cxx);
    os << indent << "}\n";
  }
}

// Build the namespace tree of `using ::ns::Entity;` re-export declarations:
// public entities are always exported; internal entities only when listed in
// reachable_internal (consumer- or macro-reachable). Entities whose FQN is in
// extern_cxx are cross-module friend-declared `extern "C++"` shared entities and
// are re-exported with matching linkage.
// A header wrapper re-exports entities that all belong to the global module —
// each is declared in a header included in the global module fragment — so the
// body is emitted as one `export extern "C++"` block, which attaches the
// re-exports there. `export` has to sit outside the linkage block rather than
// inside it. Entities with C language linkage cannot live in that block and get
// an `export extern "C"` one of their own.
//
// What the block does NOT do is make an entity with internal linkage
// re-exportable. No using-declaration can name one, wrapped or not; the
// constants module is what carries those across (see collect_constants).
void emit_wrapper_body(llvm::raw_ostream &os, const EntityModel &model,
                       llvm::StringRef internal_filter,
                       const std::vector<std::string> &reachable_internal,
                       const std::set<std::string> &extern_cxx) {
  std::regex re(internal_filter.str());
  std::set<std::string> reachable(reachable_internal.begin(),
                                   reachable_internal.end());

  NsNode cxx_root, c_root;
  bool any_c = false;
  for (const auto &item : model.items) {
    if (!wrapper_visible(item, re, reachable)) continue;
    // No using-declaration can name an entity with internal or no linkage —
    // `export using ::ns::X;` is ill-formed and GCC says so outright:
    //
    //   error: exporting 'const char ns::X' that does not have external
    //          linkage
    //
    // The ones with a value worth keeping leave through the constants module
    // instead; the rest (a `static` function, a mutable `static` global) have
    // nothing a second translation unit could be given.
    if (item.no_external_linkage) continue;
    if (item.c_language_linkage) {
      any_c = true;
      insert(c_root, item);
    } else {
      insert(cxx_root, item);
    }
  }

  os << "export extern \"C++\" {\n";
  emit_tree(os, cxx_root, "", 0, /*export_using=*/false, extern_cxx);
  os << "}\n";

  if (any_c) {
    os << "\nexport extern \"C\" {\n";
    emit_tree(os, c_root, "", 0, /*export_using=*/false, extern_cxx);
    os << "}\n";
  }
}

} // namespace

// A header's `const`/`constexpr` variables and its unnamed enums' enumerators
// have internal or no linkage, and no using-declaration may name one. Dropping
// them would take part of the library's API away, so they travel as values
// instead — which takes three modules, because the unit that reads a value and
// the unit that publishes it under the original name cannot be the same one:
//
//   <lib>.constants   includes the headers, and exports a COPY of each such
//                     value under a private namespace. It cannot publish
//                     `ns::X` itself: the header already declares that name
//                     here, and a second declaration would collide.
//   <lib>.main        includes the headers, and re-exports by using-declaration
//                     everything that has a linkage to name.
//   <lib>             includes nothing, so `ns::X` is free: it imports the
//                     other two and declares the real names from the copies.
//
// A library with no such constants gets `<lib>` alone, as before.
export std::string constants_module_name(llvm::StringRef base) {
  return base.str() + ".constants";
}

export std::string main_module_name(llvm::StringRef base) {
  return base.str() + ".main";
}

// Where the copies live until the facade renames them. Dots are not namespace
// syntax, so a dotted module name folds to underscores.
export std::string constants_namespace(llvm::StringRef base) {
  std::string ns = base.str();
  std::ranges::replace(ns, '.', '_');
  return ns + "_constants";
}

// Whether this model needs the three-module split at all.
export bool wrapper_needs_constants(
    const EntityModel &model,
    llvm::StringRef internal_filter = kDefaultInternalFilter,
    const std::vector<std::string> &reachable_internal = {}) {
  return !collect_constants(model, internal_filter, reachable_internal).empty();
}

export std::string generate_constants_cc(
    llvm::StringRef base,
    const std::vector<std::string> &headers,
    const EntityModel &model,
    llvm::StringRef internal_filter = kDefaultInternalFilter,
    const std::vector<std::string> &reachable_internal = {}) {
  std::string result;
  llvm::raw_string_ostream os(result);

  os << "module;\n";
  for (const auto &h : headers) os << "#include <" << h << ">\n";
  os << "\nexport module " << constants_module_name(base) << ";\n\n";

  // Reading a const object with internal linkage whose value is usable in
  // constant expressions is not an exposure ([basic.link] excludes exactly
  // this), and the value is all that leaves. Clang reports it anyway.
  os << "#ifdef __clang__\n"
     << "#pragma clang diagnostic ignored \"-WTU-local-entity-exposure\"\n"
     << "#endif\n\n";

  emit_constants(os, collect_constants(model, internal_filter,
                                       reachable_internal),
                 constants_namespace(base), "");
  return result;
}

// The facade over the other two. It includes no library header, which is the
// whole point: every name it declares is free here.
export std::string generate_facade_cc(
    llvm::StringRef base,
    const EntityModel &model,
    llvm::StringRef internal_filter = kDefaultInternalFilter,
    const std::vector<std::string> &reachable_internal = {},
    const std::vector<std::string> &macro_includes = {}) {
  std::string result;
  llvm::raw_string_ostream os(result);

  auto constants = collect_constants(model, internal_filter,
                                     reachable_internal);
  // A constant declared under a condition is republished under the same one,
  // and this unit includes nothing — so without the library's macro header
  // every such condition would be false here and the constant would silently
  // not exist.
  bool guarded = std::ranges::any_of(constants, [](const EntityItem *i) {
    return !i->guard_prefix.empty();
  });
  if (guarded && !macro_includes.empty()) {
    os << "module;\n";
    for (auto &inc : macro_includes) os << "#include \"" << inc << "\"\n";
    os << "\n";
  }

  os << "export module " << base.str() << ";\n"
     << "export import " << main_module_name(base) << ";\n"
     // Not exported: the copies are an implementation detail, and consumers
     // reach the values through the names declared just below.
     << "import " << constants_module_name(base) << ";\n\n";

  emit_constants(os, constants, "", constants_namespace(base));
  return result;
}

export std::string generate_wrapper_cc(
    llvm::StringRef module_name,
    const std::vector<std::string> &headers,
    const EntityModel &model,
    llvm::StringRef internal_filter = kDefaultInternalFilter,
    const std::vector<std::string> &reachable_internal = {},
    const std::vector<std::string> &extern_cxx_fqns = {}) {
  std::string result;
  llvm::raw_string_ostream os(result);

  os << "module;\n";
  for (const auto &h : headers) {
    os << "#include <" << h << ">\n";
  }
  os << "\nexport module " << module_name << ";\n\n";

  std::set<std::string> extern_cxx(extern_cxx_fqns.begin(),
                                    extern_cxx_fqns.end());
  emit_wrapper_body(os, model, internal_filter, reachable_internal, extern_cxx);

  return result;
}

// Generate a facade module that imports another module (e.g. the library's
// umbrella module, renamed to `<lib>.umbrella`) and re-exports only the public
// API plus the internal entities reachable from consumers/macros, via
// `using ::ns::Entity;` declarations. `import_modules` lists every library
// interface module (the umbrella plus each sub-module): the wrapper re-exports
// entities that may live in ANY of them, so each must be imported for the
// `using` declarations to resolve. `extern_cxx_fqns` lists cross-module
// friend-declared entities that live in the global module: their re-exports
// carry `extern "C++"` linkage so they bind to the defining module.
export std::string generate_import_wrapper_cc(
    llvm::StringRef module_name,
    llvm::StringRef import_name,
    const EntityModel &model,
    llvm::StringRef internal_filter = kDefaultInternalFilter,
    const std::vector<std::string> &reachable_internal = {},
    bool import_std = false,
    const std::vector<std::string> &import_modules = {},
    const std::vector<std::string> &extern_cxx_fqns = {},
    const std::vector<std::string> &macro_includes = {}) {
  std::string result;
  llvm::raw_string_ostream os(result);

  // An entity declared under a condition is re-exported under the same
  // condition, and this facade otherwise includes nothing — so every macro
  // those conditions test would be undefined here and each guarded re-export
  // would quietly vanish. Pull the library's generated macro headers into a
  // global module fragment so the conditions mean here what they mean in the
  // headers they came from.
  bool guarded = std::ranges::any_of(model.items, [](const EntityItem &i) {
    return !i.guard_prefix.empty();
  });
  if (guarded && !macro_includes.empty()) {
    os << "module;\n";
    for (auto &inc : macro_includes) os << "#include \"" << inc << "\"\n";
    os << "\n";
  }
  os << "export module " << module_name << ";\n";
  os << "import " << import_name << ";\n";
  for (auto &m : import_modules) {
    if (m == import_name.str()) continue;
    os << "import " << m << ";\n";
  }
  // std.compat (rather than std) also provides the C library's global names
  // (e.g. `size_t`) that re-exported entities may reference.
  if (import_std) os << "import std.compat;\n";
  os << "\n";

  std::set<std::string> extern_cxx(extern_cxx_fqns.begin(),
                                    extern_cxx_fqns.end());
  emit_wrapper_body(os, model, internal_filter, reachable_internal, extern_cxx);

  return result;
}

export std::string generate_companion_h(
    const EntityModel &model) {
  std::string result;
  llvm::raw_string_ostream os(result);

  os << "#pragma once\n\n";
  for (const auto &m : model.macros) {
    os << m.body << "\n";
  }

  return result;
}
