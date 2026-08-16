module;

export module modulizer.rewrite_util;
import modulizer.naming;
import libtooling;
import std;

// Text/source-level helpers shared by the header-rewrite modules. Pure string
// and offset manipulation plus the small data structures that carry rewrite
// operations; no clang AST traversal lives here.

export struct ModPoint {
  unsigned offset;
  unsigned len = 0;   // 0 = insertion, >0 = deletion
  std::string text;   // text to insert
};

export struct MacroRec {
  std::string name;        // macro name (e.g. "FOO")
  std::string body;       // full #define line
  unsigned start_off;     // file offset of '#'
  unsigned end_off;       // file offset after newline
  bool undefed = false;   // #undef'd later → keep in .h
  // How many `#if` blocks enclose the `#define`. An `#undef` deeper than its
  // definition is a per-platform override, not a retraction: on the platforms
  // whose branch is not taken the definition still stands, so it must reach
  // the macros file.
  unsigned cond_depth = 0;
};

namespace {

// Headers that define preprocessor macros (assert, errno, SIG_*, stderr/stdout
// on some libcs) which `import std.compat;` cannot provide and which the
// used_headers filter cannot track (macros produce no Decl). They must stay in
// the GMF whenever they are reachable so the module body and impl units can use
// the macros.
const std::set<std::string> kMacroCarryingCHeaders = {
    "cassert", "assert.h", "cerrno",   "errno.h", "csignal", "signal.h",
    "cstdio",  "stdio.h",  "pthread.h",
};

}  // namespace

export std::string decl_fqn(clang::NamedDecl *d) {
  if (!d) return {};
  std::string fqn;
  for (auto *dc = d->getDeclContext(); dc; dc = dc->getParent()) {
    if (auto *nd = llvm::dyn_cast<clang::NamespaceDecl>(dc)) {
      if (!nd->isAnonymousNamespace()) fqn = nd->getNameAsString() + "::" + fqn;
    }
  }
  fqn += d->getNameAsString();
  return fqn;
}

// A partial or explicit specialization. [module.interface]/3 forbids an
// export-declaration from declaring one: it is exported with its primary
// template, so no export marker may be attached to it.
export bool is_specialization(const clang::Decl *d) {
  if (llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(d) ||
      llvm::isa<clang::VarTemplatePartialSpecializationDecl>(d))
    return true;
  if (auto *rd = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(d))
    return rd->getSpecializationKind() ==
           clang::TemplateSpecializationKind::TSK_ExplicitSpecialization;
  if (auto *fd = llvm::dyn_cast<clang::FunctionDecl>(d))
    return fd->getTemplateSpecializationKind() ==
           clang::TemplateSpecializationKind::TSK_ExplicitSpecialization;
  if (auto *vd = llvm::dyn_cast<clang::VarDecl>(d))
    return vd->getTemplateSpecializationKind() ==
           clang::TemplateSpecializationKind::TSK_ExplicitSpecialization;
  return false;
}

export bool is_ident_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

export bool looks_like_guard_name(llvm::StringRef name) {
  return name.starts_with("_") || name.ends_with("_H") ||
         name.ends_with("_H_") || name.ends_with("_HPP");
}

export unsigned template_header_start(llvm::StringRef text, unsigned pos) {
  unsigned depth = 0;
  unsigned i = pos;
  while (i > 0) {
    char c = text[i - 1];
    if (c == '>') {
      ++depth;
    } else if (c == '<') {
      if (depth == 0) break;
      --depth;
      if (depth == 0) break;
    }
    --i;
  }
  if (i == 0 || depth != 0) return pos;
  // i points just past the matching '<'; skip spaces before it.
  auto q = i - 1;
  while (q > 0 && (text[q - 1] == ' ' || text[q - 1] == '\t')) --q;
  if (q < 8 || std::string_view(text.data() + q - 8, 8) != "template")
    return pos;
  return q - 8;
}

export bool find_extern_spec(llvm::StringRef text, unsigned from,
                             unsigned &start, unsigned &end) {
  unsigned scan = from;
  auto try_extern = [&]() {
    while (scan < text.size() && (text[scan] == ' ' || text[scan] == '\t'))
      ++scan;
    // A whole token: `externalFoo x;` starts with the same six characters and
    // must not have them cut out of it.
    if (text.size() - scan >= 6 &&
        std::string_view(text.data() + scan, 6) == "extern" &&
        (text.size() - scan == 6 || !is_ident_char(text[scan + 6]))) {
      start = scan;
      scan += 6;
      while (scan < text.size() && (text[scan] == ' ' || text[scan] == '\t'))
        ++scan;
      end = scan;
      return true;
    }
    return false;
  };
  if (try_extern()) return true;
  // Skip a leading attribute-macro identifier and retry.
  while (scan < text.size() && is_ident_char(text[scan])) ++scan;
  return try_extern();
}

export std::string find_replacing_module(
    const std::string &path,
    const std::map<std::string, std::set<std::string>> &module_replaces) {
  std::string replaced_mod;
  std::size_t replaced_size = 0;
  for (auto &[mod, set] : module_replaces) {
    if (!set.count(path)) continue;
    if (replaced_mod.empty() || set.size() < replaced_size) {
      replaced_mod = mod;
      replaced_size = set.size();
    }
  }
  return replaced_mod;
}

export bool keep_transitive_system_include(const std::string &resolved,
                                           const std::string &path,
                                           const std::set<std::string> &used_headers) {
  if (!resolved.empty() &&
      (used_headers.count(resolved) || kMacroCarryingCHeaders.count(path)))
    return true;
  return std::filesystem::path(path).filename() == "version";
}

export std::string format_import(const std::string &path,
                                 const std::string &module,
                                 const std::set<std::string> &macro_modules,
                                 InternalMode internal_mode) {
  if (!macro_modules.count(module) &&
      is_internal_module(path, module, internal_mode))
    return std::format("import {};", module);
  return std::format("export import {};", module);
}

export std::size_t prev_line_start(const std::string &src,
                                   std::size_t line_start) {
  if (line_start == 0 || src[line_start - 1] != '\n') return line_start;
  if (line_start < 2) return 0;
  auto pnl = src.rfind('\n', line_start - 2);
  return pnl == std::string_view::npos ? 0 : pnl + 1;
}

export unsigned preceding_comment_start(const std::string &src,
                                        unsigned hash_off) {
  auto nl = src.rfind('\n', hash_off - 1);
  std::size_t define_line = nl == std::string_view::npos ? 0 : nl + 1;
  // Walk backwards over blank lines above the define.
  std::size_t pos = define_line;
  for (;;) {
    auto prev = prev_line_start(src, pos);
    if (prev >= pos) break;
    std::string_view line(src.data() + prev, pos - prev - 1);
    auto ns = line.find_first_not_of(" \t\r\n");
    pos = prev;
    if (ns != std::string_view::npos) break;  // first non-blank line above
  }
  std::string_view line(src.data() + pos);
  auto eol = line.find('\n');
  std::string_view l = eol == std::string_view::npos ? line : line.substr(0, eol);
  auto ns = l.find_first_not_of(" \t");
  if (ns == std::string_view::npos || l[ns] != '/' || ns + 1 >= l.size())
    return 0;
  if (l[ns + 1] == '/') {
    // `//` comment: extend upward over contiguous `//` lines.
    unsigned start = (unsigned)pos;
    std::size_t walk = pos;
    for (;;) {
      auto prev = prev_line_start(src, walk);
      if (prev >= walk) break;
      std::string_view pl(src.data() + prev, walk - prev - 1);
      auto pns = pl.find_first_not_of(" \t\r\n");
      if (pns == std::string_view::npos) break;  // blank line ends the comment
      if (pl[pns] != '/' || pns + 1 >= pl.size() || pl[pns + 1] != '/') break;
      start = (unsigned)prev;
      walk = prev;
    }
    return start;
  }
  if (l[ns + 1] == '*') {
    // `/* */` block comment: locate the `/*` opener by scanning backward.
    std::size_t scan = pos + ns;
    while (scan > 0) {
      if (src[scan - 1] == '/' && src[scan] == '*') return (unsigned)(scan - 1);
      --scan;
    }
    return (unsigned)(pos + ns);
  }
  return 0;
}

export std::string extract_file_header_comment(const std::string &src) {
  std::string out;
  std::size_t pos = 0;
  bool in_block = false;
  while (pos < src.size()) {
    auto nl = src.find('\n', pos);
    std::string_view line = nl == std::string_view::npos
                                ? std::string_view(src).substr(pos)
                                : std::string_view(src).substr(pos, nl - pos);
    auto s = line.find_first_not_of(" \t\r");
    bool blank = s == std::string_view::npos;
    if (in_block) {
      out += line;
      out += '\n';
      if (line.find("*/") != std::string_view::npos) in_block = false;
    } else if (blank) {
      // Skip leading blank lines; keep blank lines between comment paragraphs
      // (trailing blank lines are trimmed below).
      if (!out.empty()) {
        out += line;
        out += '\n';
      }
    } else {
      auto c = line[s];
      if (c == '/' && s + 1 < line.size() && line[s + 1] == '/') {
        out += line;
        out += '\n';
      } else if (c == '/' && s + 1 < line.size() && line[s + 1] == '*') {
        out += line;
        out += '\n';
        if (line.find("*/") == std::string_view::npos) in_block = true;
      } else {
        break;  // first code/directive line ends the header comment
      }
    }
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  // Trim trailing blank lines.
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
    out.pop_back();
  return out;
}

// Remove the `extern` storage-class specifier from copied declaration text, so
// the declaration can be wrapped in `extern "C++"` — a specifier inside a
// linkage-specification is ill-formed. The keyword follows a leading
// `template <...>` header, which is skipped by matching its angle brackets; a
// scan that ends up somewhere unexpected simply finds no keyword and leaves the
// text alone. The caller decides whether the declaration has one to begin with
// (a question for the AST, not for the text). Returns whether one was removed.
export bool strip_storage_extern(std::string &text) {
  std::size_t from = 0;
  auto first = text.find_first_not_of(" \t\r\n");
  if (first != std::string::npos &&
      std::string_view(text).substr(first).starts_with("template")) {
    auto lt = text.find('<', first + 8);
    if (lt == std::string::npos) return false;
    int depth = 0;
    for (from = lt; from < text.size(); ++from) {
      if (text[from] == '<') {
        ++depth;
      } else if (text[from] == '>' && --depth == 0) {
        ++from;
        break;
      }
    }
    if (depth != 0) return false;
  }
  // The keyword commonly begins the line after the parameter list, and
  // find_extern_spec scans no further than a newline.
  while (from < text.size() && (text[from] == ' ' || text[from] == '\t' ||
                                text[from] == '\n' || text[from] == '\r'))
    ++from;
  unsigned start = 0, end = 0;
  if (!find_extern_spec(text, from, start, end)) return false;
  text.erase(start, end - start);
  return true;
}

export void strip_function_default_args(std::string &text) {
  auto lp = text.find('(');
  if (lp == std::string::npos) return;
  int paren = 0;
  for (std::size_t i = lp; i < text.size(); ++i) {
    char c = text[i];
    if (c == '(') { ++paren; continue; }
    if (c == ')') {
      if (paren == 1) return;  // end of the parameter list
      --paren;
      continue;
    }
    if (paren != 1) continue;
    // A top-level '=' starting a default argument (not part of a token like
    // "==" or ">=").
    if (c == '=') {
      bool ok = i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t' ||
                text[i - 1] == '\n' || text[i - 1] == '\r' ||
                text[i - 1] == ',' || text[i - 1] == '(';
      if (!ok) continue;
      // Find the end: a top-level ',' or ')' (the one closing the parameter
      // list), accounting for nested parens/brackets/braces in the default
      // expression.
      int depth = 0;
      std::size_t j = i + 1;
      for (; j < text.size(); ++j) {
        char d = text[j];
        if (d == '(' || d == '[' || d == '{') {
          ++depth;
        } else if (d == ')' || d == ']' || d == '}') {
          if (depth == 0) break;
          --depth;
        } else if (d == ',' && depth == 0) {
          break;
        }
      }
      text.erase(i, j - i);
      --i;
    }
  }
}

export std::string apply_mods(std::string src, std::vector<ModPoint> &mods) {
  // Multiple entities expanded from the same macro body share one spelling
  // location (e.g. every `DECLARE_KIND_(...)` expands to a `struct
  // KindOf<type>` whose begin is the macro body). Their export markers are
  // identical insertions at the same offset; keep just one so the expansion
  // isn't flooded with `export export export ...`.
  {
    std::set<std::tuple<int, int, std::string>> seen;
    std::vector<ModPoint> dedup;
    dedup.reserve(mods.size());
    for (auto &m : mods) {
      auto key = std::make_tuple(m.offset, m.len, m.text);
      if (seen.insert(key).second) dedup.push_back(m);
    }
    mods = std::move(dedup);
  }
  // A stable sort preserves push order for mods at the same offset: a
  // deletion is pushed before the insertion it pairs with (e.g. replacing
  // `extern ` with `extern "C++" `), so the deletion applies first.
  std::ranges::stable_sort(
      mods, [](const ModPoint &a, const ModPoint &b) { return a.offset > b.offset; });
  for (auto &m : mods) {
    if (m.len > 0) {
      // A deletion, or — with text — a replacement: `extern` giving way to the
      // macro that puts it back wherever the tree is built wrapped.
      if (m.offset + m.len <= src.size()) {
        src.erase(m.offset, m.len);
        if (!m.text.empty()) src.insert(m.offset, m.text);
      }
    } else {
      if (m.offset <= src.size())
        src.insert(m.offset, m.text);
    }
  }
  return src;
}
