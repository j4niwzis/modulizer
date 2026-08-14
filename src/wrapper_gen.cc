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

void insert(NsNode &root, const EntityItem &item, const std::regex &re,
            const std::set<std::string> &reachable, std::size_t depth = 0) {
  if (is_dependent_name(item.name)) return;
  if (depth >= item.ns_path.size()) {
    if (!has_internal_path(item.ns_path, re))
      root.items.push_back(&item);
    else {
      auto fqn = fqn_of(item.ns_path, item.name);
      for (auto &r : reachable) {
        if (r == fqn || fqn.ends_with("::" + r)) {
          root.items.push_back(&item);
          break;
        }
      }
    }
    return;
  }
  insert(root.children[item.ns_path[depth]], item, re, reachable, depth + 1);
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
// re-exports there. That is also what makes entities with internal linkage
// re-exportable: a plain exported using declaration naming a `const` variable
// at namespace scope is rejected outright, and `export` has to sit outside the
// linkage block rather than inside it. Entities with C language linkage cannot
// live in that block and get an `export extern "C"` one of their own.
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
    if (item.c_language_linkage) {
      any_c = true;
      insert(c_root, item, re, reachable);
    } else {
      insert(cxx_root, item, re, reachable);
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
    const std::vector<std::string> &extern_cxx_fqns = {}) {
  std::string result;
  llvm::raw_string_ostream os(result);

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
