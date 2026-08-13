module;

export module modulizer.macro_analyzer;
import modulizer.analyzer;
import modulizer.util;
import libtooling;
import std;

export struct MacroReachability {
  std::vector<std::string> reachable_fqns;
  std::vector<std::pair<std::string, std::vector<std::string>>> wildcards;
  std::vector<std::string> suppressed;
};

namespace {

bool is_internal_ns(const std::vector<std::string> &path,
                    const std::regex &re) {
  for (auto &s : path) {
    if (std::regex_match(s, re)) return true;
  }
  return false;
}

// ---- token helpers --------------------------------------------------

bool is_cpp_keyword(const std::string &tok) {
  static const std::unordered_set<std::string> kKeywords = {
    "alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch",
    "char", "char8_t", "char16_t", "char32_t", "class", "const", "consteval",
    "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
    "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
    "else", "enum", "explicit", "export", "extern", "false", "float", "for",
    "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace",
    "new", "noexcept", "nullptr", "operator", "private", "protected", "public",
    "register", "reinterpret_cast", "requires", "return", "short", "signed",
    "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
    "template", "this", "thread_local", "throw", "true", "try", "typedef",
    "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
    "volatile", "wchar_t", "while", "and", "or", "not", "xor", "bitand",
    "bitor", "compl", "not_eq", "and_eq", "or_eq", "xor_eq"
  };
  return kKeywords.count(tok) != 0;
}

bool is_identifier(const std::string &tok) {
  if (tok.empty()) return false;
  if (is_cpp_keyword(tok)) return false;
  return std::isalpha(static_cast<unsigned char>(tok[0])) || tok[0] == '_';
}

bool is_macro_param(const std::string &tok,
                     const std::vector<std::string> &params) {
  for (auto &p : params) {
    if (p == tok) return true;
  }
  return false;
}

// ---- qualified-name extraction from token stream --------------------

// Scan tokens for sequences like  id :: id [:: id ...]
// Returns vector of fully-qualified names found.
std::vector<std::string> extract_fqns(
    const std::vector<std::string> &tokens,
    const std::vector<std::string> &params) {
  std::vector<std::string> result;
  std::vector<std::string> buf;
  bool has_tokpaste = false;

  auto flush = [&]() {
    if (buf.size() >= 2) {
      std::string fqn;
      for (std::size_t i = 0; i < buf.size(); ++i) {
        if (i > 0) fqn += "::";
        fqn += buf[i];
      }
      bool has_param = false;
      for (auto &seg : buf) {
        if (is_macro_param(seg, params)) { has_param = true; break; }
      }
      if (!has_param)
        result.push_back(std::move(fqn));
    }
    buf.clear();
    has_tokpaste = false;
  };

  for (std::size_t i = 0; i < tokens.size(); ++i) {
    auto &t = tokens[i];
    if (t == "##") {
      has_tokpaste = true;
      continue;
    }
    if (t == "::") {
      if (!buf.empty() && is_identifier(buf.back()) &&
          i + 1 < tokens.size() && is_identifier(tokens[i + 1])) {
        buf.push_back(t);
        buf.push_back(tokens[++i]);
        continue;
      }
      flush();
      continue;
    }
    if (is_identifier(t)) {
      buf.push_back(t);
      continue;
    }
    flush();
  }
  flush();
  return result;
}

// ---- token-paste wildcard detection ---------------------------------

// Detect patterns like:  detail :: foo_ ## param
// Produces wildcard: ("foo_", ["detail"])
std::vector<std::pair<std::string, std::vector<std::string>>>
extract_wildcards(const std::vector<std::string> &tokens,
                  const std::vector<std::string> &params) {
  std::vector<std::pair<std::string, std::vector<std::string>>> result;
  std::vector<std::string> ns;     // current namespace path
  std::string prefix_before_hash;  // identifier before ##

  auto flush = [&]() {
    ns.clear();
    prefix_before_hash.clear();
  };

  for (std::size_t i = 0; i < tokens.size(); ++i) {
    auto &t = tokens[i];
    if (t == "::") {
      if (!prefix_before_hash.empty()) {
        ns.push_back(std::move(prefix_before_hash));
      }
      continue;
    }
    if (is_identifier(t)) {
      if (i + 1 < tokens.size() && tokens[i + 1] == "::") {
        ns.push_back(t);
        continue;
      }
      if (i + 1 < tokens.size() && tokens[i + 1] == "##" &&
          i + 2 < tokens.size() && is_identifier(tokens[i + 2]) &&
          is_macro_param(tokens[i + 2], params)) {
        // token ## param — wildcard: token_*
        if (!ns.empty()) {
          result.push_back({t, ns});
        }
        i += 2; // skip ## and param
        flush();
        continue;
      }
      // Could also be: param ## token (reverse order)
      if (is_macro_param(t, params) &&
          i + 1 < tokens.size() && tokens[i + 1] == "##" &&
          i + 2 < tokens.size() && is_identifier(tokens[i + 2])) {
        if (!ns.empty())
          result.push_back({tokens[i + 2], ns});
        i += 2;
        flush();
        continue;
      }
      flush();
      continue;
    }
    flush();
  }
  return result;
}

// ---- macro call graph ------------------------------------------------

// Build a map: macro_name → index in input vector.
// Detect direct dependencies: if macro A's body contains identifier B
// which is another macro's name, add edge A→B.
std::vector<std::vector<std::size_t>> build_call_graph(
    const std::vector<EntityMacro> &macros) {
  std::map<std::string, std::size_t, std::less<>> name_to_idx;
  for (std::size_t i = 0; i < macros.size(); ++i)
    name_to_idx[macros[i].name] = i;

  std::vector<std::vector<std::size_t>> deps(macros.size());
  for (std::size_t i = 0; i < macros.size(); ++i) {
    for (auto &tok : macros[i].tokens) {
      if (!is_identifier(tok)) continue;
      if (is_macro_param(tok, macros[i].params)) continue;
      auto it = name_to_idx.find(tok);
      if (it != name_to_idx.end() && it->second != i)
        deps[i].push_back(it->second);
    }
  }
  return deps;
}

// ---- public macro detection ------------------------------------------

bool is_public_macro(const EntityMacro &m) {
  if (m.name.empty()) return false;
  if (m.name[0] == '_') return false;
  // Skip include guards (ALL_CAPS with _H_ or _H suffix)
  if (m.name.ends_with("_H_") || m.name.ends_with("_H")) {
    bool all_upper = true;
    for (char c : m.name) {
      if (std::islower(static_cast<unsigned char>(c))) { all_upper = false; break; }
    }
    if (all_upper) return false;
  }
  return true;
}

} // namespace

// ---- main entry point -------------------------------------------------

export MacroReachability analyze_macro_reachability(
    const std::vector<EntityMacro> &macros,
    [[maybe_unused]] const EntityModel &model,
    std::string_view internal_filter_str) {

  MacroReachability r;
  auto internal_re = std::regex(std::string(internal_filter_str));

  // 1. Identify public macros
  std::vector<std::size_t> public_idx;
  for (std::size_t i = 0; i < macros.size(); ++i) {
    if (is_public_macro(macros[i]))
      public_idx.push_back(i);
  }

  // 2. Build call graph
  auto deps = build_call_graph(macros);

  // 3. Transitive closure: start from public macros, BFS
  std::set<std::size_t> visited(public_idx.begin(), public_idx.end());
  std::vector<std::size_t> queue = public_idx;
  while (!queue.empty()) {
    std::size_t cur = queue.back(); queue.pop_back();
    for (std::size_t next : deps[cur]) {
      if (visited.insert(next).second)
        queue.push_back(next);
    }
  }

  // 4. Extract qualified names and wildcards from all reachable macros
  std::set<std::string> fqn_set;
  for (std::size_t idx : visited) {
    auto &m = macros[idx];
    for (auto &fqn : extract_fqns(m.tokens, m.params))
      fqn_set.insert(fqn);
    for (auto &wc : extract_wildcards(m.tokens, m.params))
      r.wildcards.push_back(wc);
  }

  // 5. Filter: only keep FQNs in internal namespaces
  // Also add parent scope prefixes (ancestor entities are reachable too)
  std::set<std::string> internal_fqns;
  for (auto &fqn : fqn_set) {
    auto parts = split_on(fqn, ':');
    if (parts.size() >= 2) {
      // Find the shallowest prefix whose namespace path contains an
      // internal component; that is the entity that must be exported.
      for (std::size_t end = 2; end <= parts.size(); ++end) {
        std::vector<std::string> ns_path(parts.begin(), parts.begin() + end - 1);
        if (is_internal_ns(ns_path, internal_re)) {
          std::string pfqn;
          for (std::size_t k = 0; k < end; ++k) {
            if (k > 0) pfqn += "::";
            pfqn += parts[k];
          }
          internal_fqns.insert(std::move(pfqn));
          break;
        }
      }
    }
  }
  r.reachable_fqns = std::ranges::to<std::vector>(internal_fqns);

  return r;
}

// ---- post-processing ---------------------------------------------------

export void expand_overloads(
    MacroReachability &r,
    const EntityModel &model) {
  // Collect function names that are reachable
  std::set<std::string> reachable_funcs;
  for (auto &fqn : r.reachable_fqns) {
    // Check if this FQN matches a function in the model
    for (auto &item : model.items) {
      if (item.kind != EntityItem::kFunction) continue;
      auto item_fqn = fqn_of(item.ns_path, item.name);
      if (item_fqn == fqn) {
        // Found a function — add all overloads with same namespace+name
        for (auto &other : model.items) {
          if (other.kind != EntityItem::kFunction) continue;
          if (other.ns_path != item.ns_path) continue;
          if (other.name != item.name) continue;
          auto ofqn = fqn_of(other.ns_path, other.name);
          // Use pointer identity to distinguish overloads
          if (&other != &item)
            reachable_funcs.insert(ofqn);
        }
      }
    }
  }
  r.reachable_fqns.append_range(reachable_funcs);
}

// Transitive type closure (design doc step 6g): a reachable entity exposes the
// library types its declaration references — a function's return/parameter
// types, a class's base classes and public-member signatures, a variable's
// type, an alias's underlying type. Each such type is itself an entity whose
// declaration may mention further types, so the closure iterates to a fixed
// point.
//
// Only entities in an internal namespace join the reachable set: public
// entities are exported by default and would be redundant there. Public types
// are still traversed as intermediates (their public methods may return
// internal types that consumers reach through them).
export void expand_transitive_types(
    MacroReachability &r,
    const EntityModel &model,
    std::string_view internal_filter = kDefaultInternalFilter) {
  auto internal_re = std::regex(std::string(internal_filter));

  // Index model items by FQN so a reachable name can be expanded to the types
  // its declaration mentions.
  std::map<std::string, std::vector<const EntityItem *>, std::less<>> by_fqn;
  for (auto &item : model.items)
    by_fqn[fqn_of(item.ns_path, item.name)].push_back(&item);

  auto is_internal_fqn = [&](const std::string &fqn) {
    auto parts = split_on(fqn, ':');
    if (parts.size() < 2) return false;
    std::vector<std::string> ns(parts.begin(), parts.end() - 1);
    return is_internal_ns(ns, internal_re);
  };

  std::set<std::string> added;
  std::set<std::string> visited;
  std::vector<std::string> worklist = r.reachable_fqns;
  while (!worklist.empty()) {
    auto fqn = std::move(worklist.back());
    worklist.pop_back();
    if (!visited.insert(fqn).second) continue;
    auto it = by_fqn.find(fqn);
    if (it == by_fqn.end()) continue;
    for (auto *item : it->second) {
      for (auto &tr : item->type_refs) {
        if (visited.count(tr)) continue;
        worklist.push_back(tr);
        if (is_internal_fqn(tr)) added.insert(tr);
      }
    }
  }
  r.reachable_fqns.append_range(added);
}
