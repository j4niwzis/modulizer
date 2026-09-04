module;

#include <unistd.h>

export module modulizer.include_analysis;
import libtooling;
import std;

namespace {

// Read a full #if guard line including backslash continuations. Returns the
// joined guard text (ending in '\n'); sets *multiline if a continuation was
// present.
std::string read_guard_line(std::string_view src, std::size_t line_start,
                            std::size_t &nl, bool *multiline) {
  std::string out;
  std::size_t pos = line_start;
  while (pos < src.size()) {
    auto end = src.find('\n', pos);
    if (end == std::string_view::npos) end = src.size();
    auto l = src.substr(pos, end - pos);
    bool cont = !l.empty() && l.back() == '\\';
    if (cont) {
      *multiline = true;
      l = l.substr(0, l.size() - 1);
    }
    out += l;
    if (cont) {
      out += ' ';
    } else {
      out += '\n';
      nl = end;
      break;
    }
    pos = end + 1;
  }
  return out;
}

std::string negate_guard(std::string_view line) {
  std::string_view s = line;
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s = s.substr(0, s.size() - 1);
  std::string out;
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) out += s[i++];
  out += '#';
  ++i;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) out += s[i++];
  auto rest = s.substr(i);
  std::string neg;
  if (rest.starts_with("ifdef"))
    neg = "ifndef" + std::string(rest.substr(5));
  else if (rest.starts_with("ifndef"))
    neg = "ifdef" + std::string(rest.substr(6));
  else if (rest.starts_with("if"))
    neg = "if !(" + std::string(rest.substr(2)) + ")";
  else
    neg = std::string(rest);
  return out + neg + "\n";
}

} // namespace

export struct DirectiveParts {
  std::string keyword;
  std::string after;    // text following the keyword, leading spaces consumed
  std::size_t hash_pos; // offset of '#' within the line
};

// Parse a preprocessor directive line into keyword + trailing text.
// `skip_hash_ws` also skips spaces between '#' and the keyword. Leave it on
// unless a pass genuinely needs `#` and the keyword adjacent: `# if` and
// `#   define` are ordinary C++, common in libraries that indent nested
// conditionals, and a pass that does not recognise them silently treats the
// line as not a directive at all. `keyword_ends_crlf` includes '\r'/'\n' as
// keyword terminators. Returns nullopt for non-directive lines.
export std::optional<DirectiveParts> parse_directive(
    std::string_view line, bool skip_hash_ws = true,
    bool keyword_ends_crlf = true) {
  auto ns = line.find_first_not_of(" \t");
  if (ns == std::string_view::npos || line[ns] != '#') return std::nullopt;
  auto rest = line.substr(ns + 1);
  if (skip_hash_ws) {
    auto after_hash = rest.find_first_not_of(" \t");
    if (after_hash == std::string_view::npos) return std::nullopt;
    rest = rest.substr(after_hash);
  }
  auto terminator = keyword_ends_crlf ? " \t\r\n" : " \t";
  auto sp = rest.find_first_of(terminator);
  auto keyword = sp == std::string_view::npos ? rest : rest.substr(0, sp);
  if (keyword.empty()) return std::nullopt;
  auto after = sp == std::string_view::npos
                   ? std::string{}
                   : std::string(rest.substr(sp + 1));
  return DirectiveParts{std::string(keyword), std::move(after), ns};
}

// What a preprocessor conditional directive does to the conditional nesting:
// opens a block (`#if`/`#ifdef`/`#ifndef`), closes one (`#endif`), or switches
// branch within the current one (`#else`/`#elif`/`#elifdef`/`#elifndef`).
export enum class PreprocessorConditional { kOpen, kClose, kBranch };

// Classify a line as a preprocessor conditional directive; nullopt for every
// other line. Callers track conditional nesting depth with it — code at depth
// zero is compiled unconditionally, code below it is not.
export std::optional<PreprocessorConditional> preprocessor_conditional_kind(
    std::string_view line) {
  auto d = parse_directive(line);
  if (!d) return std::nullopt;
  const auto &kw = d->keyword;
  if (kw == "if" || kw == "ifdef" || kw == "ifndef")
    return PreprocessorConditional::kOpen;
  if (kw == "endif") return PreprocessorConditional::kClose;
  if (kw == "else" || kw == "elif" || kw == "elifdef" || kw == "elifndef")
    return PreprocessorConditional::kBranch;
  return std::nullopt;
}

// The form a consumer writes to include `header_path`: the path relative to
// whichever `-I` directory (from `extra_args`) contains it, longest match
// first, so `/opt/llvm/include/clang/AST/Decl.h` under `-I/opt/llvm/include`
// becomes `clang/AST/Decl.h`. Falls back to the path as given. A generated
// wrapper must emit this and not the path it was handed, or it bakes the
// generating machine's layout into the output.
export std::string include_form(llvm::StringRef header_path,
                                const std::vector<std::string> &extra_args) {
  auto path = std::filesystem::path(header_path.str()).lexically_normal();
  std::string best;
  auto consider = [&](std::string_view dir) {
    if (dir.empty()) return;
    auto d = std::filesystem::path(dir).lexically_normal();
    auto rel = path.lexically_relative(d).string();
    if (rel.empty() || rel.starts_with("..")) return;
    if (best.empty() || rel.size() < best.size()) best = rel;
  };
  for (std::size_t i = 0; i < extra_args.size(); ++i) {
    if (extra_args[i] == "-I" && i + 1 < extra_args.size())
      consider(extra_args[i + 1]);
    else if (extra_args[i].starts_with("-I"))
      consider(std::string_view(extra_args[i]).substr(2));
  }
  return best.empty() ? header_path.str() : best;
}

export struct IncludeRef {
  std::string path;
  bool is_quoted;
  std::size_t hash_pos; // offset of '#' within the line
};

// Parse a single `#include "path"` / `#include <path>` line. Returns nullopt
// when the line is not an include directive or has no path. `skip_hash_ws`
// must mirror the caller's directive-parsing rules, and every pass that works
// against the list `parse_includes` produced has to match it: `# include`,
// indented under a conditional, is an include to one side and plain text to
// the other.
export std::optional<IncludeRef> parse_include_line(
    std::string_view line, bool skip_hash_ws = true) {
  auto d = parse_directive(line, skip_hash_ws, /*keyword_ends_crlf=*/false);
  if (!d || d->keyword != "include") return std::nullopt;
  auto open = d->after.find_first_not_of(" \t");
  if (open == std::string_view::npos) return std::nullopt;
  bool is_quoted = d->after[open] == '"';
  char closer = is_quoted ? '"' : '>';
  auto close = d->after.find(closer, open + 1);
  if (close == std::string_view::npos) return std::nullopt;
  return IncludeRef{std::string(d->after.substr(open + 1, close - open - 1)),
                    is_quoted, d->hash_pos};
}

// The C++ standard library headers: the exact set a program may include
// directly. Everything else a standard-library header pulls in is that
// implementation's private business (`bits/…`, `__algorithm/…`). Also the
// headers `import std;` provides: with --import-std a consumer's `#include`
// lines for them are dropped and replaced by a single `import std;`.
export const std::set<std::string> kStdHeaders = {
    "algorithm",      "any",            "array",        "atomic",
    "barrier",        "bit",            "bitset",       "charconv",
    "chrono",         "cassert",        "ccomplex",     "cctype",
    "cerrno",         "cfenv",          "cfloat",       "cinttypes",
    "ciso646",        "climits",        "clocale",      "cmath",
    "codecvt",        "compare",        "complex",      "concepts",
    "coroutine",      "csetjmp",        "csignal",      "cstdarg",
    "cstdbool",       "cstddef",        "cstdint",      "cstdio",
    "cstdlib",        "cstring",        "ctgmath",      "ctime",
    "cuchar",         "cwchar",         "cwctype",      "deque",
    "exception",      "expected",       "filesystem",   "format",
    "forward_list",   "fstream",        "functional",   "future",
    "initializer_list", "iomanip",      "ios",          "iosfwd",
    "iostream",       "istream",        "iterator",     "latch",
    "limits",         "list",           "locale",       "map",
    "memory",         "memory_resource", "mutex",       "new",
    "numbers",        "numeric",        "optional",     "ostream",
    "queue",          "random",         "ranges",       "ratio",
    "regex",          "scoped_allocator", "semaphore",  "set",
    "shared_mutex",   "source_location", "span",        "spanstream",
    "sstream",        "stack",          "stdexcept",    "stop_token",
    "streambuf",      "string",         "string_view",  "syncstream",
    "system_error",   "thread",         "tuple",        "type_traits",
    "typeindex",      "typeinfo",       "unordered_map", "unordered_set",
    "utility",        "valarray",       "variant",      "vector",
    "version",
};

export struct IncludeDirective {
  unsigned offset;
  unsigned end_offset;
  std::string path;
  bool is_quoted;
  std::string line;
  std::vector<std::string> guard_stack;
  bool skip_gmf = false;
  // Reached through an include the body keeps (see skip_gmf): its text is read
  // in the module purview, so what it includes is read there too.
  bool from_body_read = false;
  bool transitive = false;
  std::string parent_resolved;  // resolved path of the header that included it
  // Discovered inside a standard-library/system header (`<bits/types.h>` seen
  // through `<stdio.h>`). Such a header is an implementation detail of the
  // standard library it came from and is never emitted; `public_ancestor` is
  // the resolved path of the system header the source can legitimately include
  // to obtain it.
  bool system_internal = false;
  std::string public_ancestor;
};

export std::vector<IncludeDirective> parse_includes(const std::string &src) {
  std::vector<IncludeDirective> includes;
  std::size_t pos = 0;
  while (pos < src.size()) {
    auto nl = src.find('\n', pos);
    if (nl == std::string::npos) nl = src.size();
    auto line = src.substr(pos, nl - pos + 1);
    auto inc = parse_include_line(line, /*skip_hash_ws=*/true);
    if (inc) {
      includes.push_back({
        static_cast<unsigned>(pos + inc->hash_pos),
        static_cast<unsigned>(pos + line.size()),
        std::move(inc->path),
        inc->is_quoted,
        std::string(line)
      });
    }
    pos = nl + 1;
    if (pos >= src.size()) break;
  }
  return includes;
}

// The closing half of a macro bracket: no include guard of its own, and every
// directive in it takes a macro back. It is written to be read at one
// particular point -- after the declarations spelled in the terms its partner
// defined -- so it cannot be hoisted. Hoisted, it lands next to the half that
// opened the bracket and the terms are gone before anything uses them:
//
//   iterator_concepts.hpp:51: error: a type specifier is required for all
//     declarations, and: use of undeclared identifier 'ReadableIterator'
//
// The opening half is not like this: what it defines survives wherever it is
// read, so hoisting it is fine and only the close has to stay put.
export bool is_closing_bracket_half(const std::string &src) {
  bool saw_undef = false;
  std::size_t pos = 0;
  while (pos < src.size()) {
    auto nl = src.find('\n', pos);
    if (nl == std::string::npos) nl = src.size();
    auto line = std::string_view(src).substr(pos, nl - pos);
    pos = nl + 1;
    auto d = parse_directive(line, /*skip_hash_ws=*/true,
                             /*keyword_ends_crlf=*/true);
    if (!d) continue;
    if (d->keyword == "undef") { saw_undef = true; continue; }
    // Anything else -- a guard, a define, a conditional, an include -- means
    // this is a header that happens to undefine something, not a bracket half.
    return false;
  }
  return saw_undef;
}

export bool is_xmacro_include(const std::string &path) {
  return path.ends_with(".inc") || path.ends_with(".def");
}

export std::string read_file(llvm::StringRef path) {
  // A directory opens as a stream, and its "size" is not one: tellg answers
  // -1, which as a length is every byte there could ever be —
  //
  //   terminate called after throwing an instance of 'std::length_error'
  //     what():  basic_string::_M_create
  //
  // and an include CAN resolve to a directory: `lib/detail/type_traits` names
  // one, next to the `type_traits/negation.hpp` the header actually asked for.
  // The tellg check stands on its own for anything else that opens but cannot
  // be measured.
  std::error_code ec;
  if (!std::filesystem::is_regular_file(std::filesystem::path(path.str()), ec))
    return {};
  std::ifstream f(std::string(path), std::ios::binary | std::ios::ate);
  if (!f) return {};
  auto sz = f.tellg();
  if (sz < 0) return {};
  f.seekg(0);
  std::string content(static_cast<std::size_t>(sz), '\0');
  f.read(content.data(), sz);
  return content;
}

// The compiler's default system include directories, parsed once from
// `clang++ -E -x c++ - -v`.
export std::vector<std::string> get_system_include_dirs() {
  static std::vector<std::string> dirs;
  static bool initialized = false;
  if (initialized) return dirs;
  initialized = true;
  // Unique temp file per process: the parallel test runner runs test binaries
  // concurrently, and a shared fixed path would race (one process truncating
  // the file while another reads it), yielding an incomplete include list.
  auto tmp = std::filesystem::temp_directory_path() /
             std::format("modulizer_incdirs_{}.txt", getpid());
  std::system(std::format("clang++ -E -x c++ - -v </dev/null >{} 2>&1",
                          tmp.string()).c_str());
  std::ifstream f(tmp);
  std::string line;
  bool in_block = false;
  while (std::getline(f, line)) {
    if (in_block) {
      if (line.contains("End of search list")) break;
      auto ns = line.find_first_not_of(" \t");
      if (ns != std::string::npos) dirs.push_back(line.substr(ns));
    } else if (line.find("#include <...> search starts here:") !=
               std::string::npos) {
      in_block = true;
    }
  }
  std::filesystem::remove(tmp);
  return dirs;
}

// Whether a resolved header path lies in one of the compiler's own include
// directories, i.e. it is a standard-library or system header rather than one
// of the library being modularized.
export bool resolved_in_system_dir(const std::string &resolved) {
  auto rp = std::filesystem::path(resolved).lexically_normal().string();
  for (auto &dir : get_system_include_dirs()) {
    if (dir.empty()) continue;
    auto dp = std::filesystem::path(dir).lexically_normal().string();
    if (rp.starts_with(dp)) return true;
  }
  return false;
}

export std::string resolve_include(llvm::StringRef inc_path,
                                   llvm::StringRef header_path,
                                   const std::vector<std::string> &extra_args) {
  auto hdr_dir = std::filesystem::path(header_path.str()).parent_path();
  auto rel = hdr_dir / std::filesystem::path(inc_path.str());
  if (std::filesystem::exists(rel))
    return rel.string();

  for (std::size_t i = 0; i < extra_args.size(); ++i) {
    std::string dir;
    if (extra_args[i] == "-I" && i + 1 < extra_args.size()) {
      dir = extra_args[++i];
    } else if (extra_args[i].starts_with("-I")) {
      dir = extra_args[i].substr(2);
    }
    if (!dir.empty()) {
      auto p = std::filesystem::path(dir) / std::filesystem::path(inc_path.str());
      if (std::filesystem::exists(p))
        return p.string();
    }
  }

  // Fall back to the compiler's standard include search paths so stdlib
  // headers (e.g. <iostream>) can be resolved too.
  for (auto &dir : get_system_include_dirs()) {
    auto p = std::filesystem::path(dir) / std::filesystem::path(inc_path.str());
    if (std::filesystem::exists(p))
      return p.string();
  }
  return {};
}

// The condition a guard line tests, as an expression that can be combined into
// a larger `#if`. A trailing line comment is dropped: the expression is about
// to be parenthesised into the middle of a line, where `//` would comment out
// everything after it.
std::string guard_condition(std::string_view line) {
  std::string_view s = line;
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.remove_suffix(1);
  auto hash = s.find('#');
  if (hash == std::string_view::npos) return {};
  auto i = hash + 1;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  auto rest = s.substr(i);
  auto arg = [&](std::size_t n) {
    auto a = rest.substr(n);
    if (auto c = a.find("//"); c != std::string_view::npos) a = a.substr(0, c);
    auto ns = a.find_first_not_of(" \t");
    if (ns == std::string_view::npos) return std::string_view();
    a.remove_prefix(ns);
    while (!a.empty() && (a.back() == ' ' || a.back() == '\t'))
      a.remove_suffix(1);
    return a;
  };
  auto name = [&](std::size_t n) {
    auto a = arg(n);
    auto e = a.find_first_of(" \t");
    return e == std::string_view::npos ? a : a.substr(0, e);
  };
  if (rest.starts_with("ifdef")) return std::format("defined({})", name(5));
  if (rest.starts_with("ifndef")) return std::format("!defined({})", name(6));
  if (rest.starts_with("elif")) return std::format("({})", arg(4));
  if (rest.starts_with("if")) return std::format("({})", arg(2));
  return {};
}

// One entry of the conditional stack an include sits inside. `state` is
// 0 = if-branch, 1 = elif-branch, 2 = else-branch, 3 = multi-line (tracked for
// nesting but never rebuilt). The header's own full-file include guard
// (file_guard) is tracked but never emitted.
//
// A branch past the first is only reached when every branch before it was not,
// so `cond` (this branch's own condition) and `prior` (the ones it had to
// fail) together are what guards an include inside it.
struct GuardRec {
  std::string text;
  std::string cond;
  std::vector<std::string> prior;
  int state = 0;
  bool file_guard = false;
};

// The guard to emit for an include sitting inside this branch, or nullopt when
// the branch is the file's own include guard and guards nothing.
//
// A dispatch header — `#if <this platform> … #elif <that one> …`, each branch
// including a different implementation of the same entities — depends on the
// negated priors: without them every alternative looks unconditional and they
// all land in the same translation unit at once.
std::optional<std::string> branch_guard(const GuardRec &g) {
  // Multi-line guards are kept as spelled: read_guard_line joins the
  // continuations into a single valid line (no backslash). They were
  // previously dropped because the raw `\`-terminated text is not a valid
  // single-line expression; valid_guards filters any residual backslash guard
  // at emission time.
  if (g.state == 3) return g.text;
  if (g.file_guard) return std::nullopt;
  // The first branch is its own condition, spelled the way the source spelled
  // it. Anything later also has to say which branches it is standing in for;
  // an `#else` after a single `#if` is just that one negated, which reads
  // better as `#ifndef` than as `#if !(…)`.
  if (g.prior.empty())
    return g.state == 2 ? negate_guard(g.text) : g.text;
  if (g.prior.size() == 1 && g.state == 2) return negate_guard(g.text);
  std::string expr;
  for (auto &p : g.prior)
    expr += (expr.empty() ? "" : " && ") + std::format("!{}", p);
  if (!g.cond.empty()) expr += " && " + g.cond;
  return std::format("#if {}\n", expr);
}

export void annotate_guards(const std::string &src,
                            std::vector<IncludeDirective> &includes) {
  std::vector<GuardRec> guard_stack;
  std::optional<std::string> pending_file_guard;
  std::size_t line_start = 0;
  for (std::size_t i = 0; i < src.size(); ++i) {
    if (src[i] != '\n' && i + 1 < src.size()) continue;
    auto line = std::string_view(src).substr(line_start, i - line_start + 1);
    auto dir = parse_directive(line, /*skip_hash_ws=*/true,
                               /*keyword_ends_crlf=*/true);
    if (dir) {
      auto &kw = dir->keyword;
      if (kw == "ifdef" || kw == "ifndef" || kw == "if") {
        bool multiline = false;
        std::size_t nl = 0;
        auto full = read_guard_line(src, line_start, nl, &multiline);
        auto cond = multiline ? std::string() : guard_condition(full);
        guard_stack.push_back(
            {std::move(full), std::move(cond), {}, multiline ? 3 : 0, false});
        // The first directive at depth 0 of the form `#ifndef X` is a
        // candidate for the header's own include guard; it is confirmed
        // when `#define X` follows immediately.
        if (guard_stack.size() == 1 && kw == "ifndef" && !multiline) {
          auto after = guard_stack.back().text;
          auto msp = after.find_first_of(" \t");
          if (msp != std::string::npos) {
            auto name = after.substr(msp + 1);
            auto mend = name.find_first_of(" \t\r\n");
            if (mend != std::string::npos) name = name.substr(0, mend);
            pending_file_guard = std::string(name);
          }
        } else if (guard_stack.size() == 1) {
          pending_file_guard.reset();
        }
        if (nl >= src.size()) i = src.size() - 1;
      } else if (kw == "elif") {
        if (!guard_stack.empty() && guard_stack.back().state != 3) {
          auto &g = guard_stack.back();
          bool multiline = false;
          std::size_t nl = 0;
          auto full = read_guard_line(src, line_start, nl, &multiline);
          if (!g.cond.empty()) g.prior.push_back(g.cond);
          g.cond = multiline ? std::string() : guard_condition(full);
          g.state = g.cond.empty() ? 3 : 1;
          if (nl >= src.size()) i = src.size() - 1;
        }
      } else if (kw == "else") {
        if (!guard_stack.empty() && guard_stack.back().state != 3) {
          auto &g = guard_stack.back();
          if (!g.cond.empty()) g.prior.push_back(g.cond);
          g.cond.clear();
          g.state = 2;
        }
      } else if (kw == "define" && pending_file_guard.has_value() &&
                 !guard_stack.empty()) {
        auto after = dir->after;
        auto nsp2 = after.find_first_not_of(" \t");
        if (nsp2 != std::string::npos) after = after.substr(nsp2);
        auto mend = after.find_first_of(" \t\r\n");
        auto name = mend == std::string::npos ? after : after.substr(0, mend);
        if (name == *pending_file_guard)
          guard_stack.back().file_guard = true;
        pending_file_guard.reset();
      } else if (kw == "endif") {
        if (!guard_stack.empty()) guard_stack.pop_back();
      } else {
        // Any other directive between #ifndef X and #define X means it is
        // not a file include guard.
        pending_file_guard.reset();
      }
    }
    for (auto &inc : includes) {
      if (inc.transitive) continue;
      if (inc.offset >= line_start && inc.offset <= i && !guard_stack.empty()
          && inc.guard_stack.empty()) {
        std::vector<std::string> guards;
        for (auto &g : guard_stack)
          if (auto guard = branch_guard(g)) guards.push_back(*std::move(guard));
        inc.guard_stack = std::move(guards);
      }
    }
    line_start = i + 1;
  }
}

// Expand the include closure of a header/source body so system headers reached
// transitively through (now imported) library headers can be kept in the GMF.
// Quoted library headers become module imports; system headers resolvable via
// the include path are expanded; stdlib headers are not. Transitive includes
// are marked and appended (cycle-safe via `visited`), and annotated with their
// guard context. Shared by rewrite_header and rewrite_source.
export void expand_include_closure(
    std::vector<IncludeDirective> &includes, llvm::StringRef base_path,
    const std::vector<std::string> &extra_args) {
  std::set<std::string> visited;
  for (std::size_t idx = 0; idx < includes.size(); ++idx) {
    // By value: the loop appends to `includes` below, and a reference into the
    // vector would dangle as soon as that reallocates.
    auto inc = includes[idx];
    if (is_xmacro_include(inc.path)) continue;
    if (inc.path.contains("/custom/")) continue;
    if (!visited.insert(inc.path).second) continue;
    auto resolved = resolve_include(inc.path, base_path, extra_args);
    if (resolved.empty()) continue;
    auto dep_text = read_file(resolved);
    auto dep_includes = parse_includes(dep_text);
    annotate_guards(dep_text, dep_includes);
    // Everything reached from inside a standard-library/system header is that
    // library's own business, unless it is itself a public standard header
    // (`<string>` through `<iostream>` is one a program may include and so
    // stays a candidate). Private headers and private guard conditions
    // (`<bits/alltypes.h>`, `<__algorithm/sort.h>`, `#if __building_module(std)`)
    // must never be emitted, or the output is pinned to the exact libc++ /
    // libstdc++ / musl it was generated against. They are still walked, because
    // a declaration the body uses may be declared in one of them, and the use
    // has to be credited to the public header that owns it.
    bool inside_system =
        inc.system_internal || (!inc.is_quoted && resolved_in_system_dir(resolved));
    for (auto &di : dep_includes) {
      if (is_xmacro_include(di.path)) continue;
      di.transitive = true;
      di.parent_resolved = resolved;
      if (kStdHeaders.count(di.path)) {
        // A public standard header stays a candidate wherever it was found,
        // but the conditions it was found *under* belong to the header that
        // found it, not to this program: `<cerrno>` inside
        // `<bits/atomic_wait.h>` sits behind `#if __glibcxx_atomic_wait`, and
        // emitting that guard would pin the output to one libstdc++ version.
        // A library header's conditions travel no better — the global module
        // fragment has no `BOOST_WORKAROUND` to evaluate them with:
        //
        //   error: token is not a valid binary operator in a preprocessor
        //          subexpression
        //
        // and a standard header is always there to include, so dropping the
        // condition costs nothing either way.
        di.guard_stack.clear();
      } else if (inside_system) {
        di.system_internal = true;
        di.public_ancestor = inc.system_internal ? inc.public_ancestor : resolved;
      }
      includes.push_back(std::move(di));
    }
  }
}

// The public system headers that own a declaration the body uses but that
// declare it in one of their private headers (`size_t` in `<bits/types.h>`,
// reached through `<stddef.h>`). The private header is never emitted, so the
// use is credited to its public ancestor instead: unioned into `used_headers`,
// it keeps the ancestor in the global module fragment.
export std::set<std::string> used_public_ancestors(
    const std::vector<IncludeDirective> &includes, llvm::StringRef base_path,
    const std::vector<std::string> &extra_args,
    const std::set<std::string> &used_headers) {
  std::set<std::string> out;
  for (auto &inc : includes) {
    if (!inc.system_internal || inc.public_ancestor.empty()) continue;
    auto resolved = resolve_include(inc.path, base_path, extra_args);
    if (!resolved.empty() && used_headers.count(resolved))
      out.insert(inc.public_ancestor);
  }
  return out;
}

// Drop transitive system includes whose parent header is also emitted in the
// GMF (e.g. <sys/stat.h> also pulls in <bits/stat.h>; keeping both would
// redefine struct stat). Shared by rewrite_header and rewrite_source.
export void filter_subsumed_transitive(
    std::vector<IncludeDirective> &gmf_incs, llvm::StringRef base_path,
    const std::vector<std::string> &extra_args) {
  std::set<std::string> kept_resolved;
  for (auto &g : gmf_incs) {
    if (g.is_quoted) continue;
    auto r = resolve_include(g.path, base_path, extra_args);
    if (!r.empty()) kept_resolved.insert(r);
  }
  std::vector<IncludeDirective> kept;
  kept.reserve(gmf_incs.size());
  for (auto &g : gmf_incs) {
    // A transitive child whose parent is also emitted is subsumed by the
    // parent's textual include (musl `<bits/stat.h>` under `<sys/stat.h>`);
    // emitting both double-includes the guard-less child and redefines its
    // declarations.
    if (g.transitive && !g.is_quoted && !g.parent_resolved.empty() &&
        kept_resolved.count(g.parent_resolved))
      continue;
    kept.push_back(std::move(g));
  }
  gmf_incs = std::move(kept);
}
