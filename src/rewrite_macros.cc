module;

export module modulizer.rewrite_macros;
import modulizer.astutil;
import modulizer.include_analysis;
import modulizer.rewrite_includes;
import modulizer.rewrite_util;
import libtooling;
import std;

// Macro collection and macro-file construction for the header rewriter:
// PPCallbacks collectors, textual extraction of #define lines, override-guard
// analysis, and the generated `<stem>_macros.h` builder.

// Records a macro invocation that must be exported.

export class HeaderMacroCollector : public clang::PPCallbacks {
public:
  HeaderMacroCollector(const clang::SourceManager &sm,
                       std::vector<MacroRec> &macros)
      : sm(sm), macros(macros) {}

  void MacroDefined(const clang::Token &name,
                    const clang::MacroDirective *md) override {
    auto *mi = collectible_macro(md, sm);
    if (!mi) return;
    auto defLoc = mi->getDefinitionLoc();

    auto fileID = sm.getFileID(defLoc);
    unsigned nameOff = sm.getFileOffset(defLoc);

    const char *buf = sm.getCharacterData(
        sm.getLocForStartOfFile(fileID));
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

    // collectible_macro already rejected clang-detected header guards; this
    // additionally catches guards clang misses (a previous `#ifndef X` line).
    // Only names that look like a file include guard (a leading `_` or a
    // `_H`/`_H_` suffix) are rejected; a default-value macro guarded by
    // `#ifndef X`/`#define X` (e.g. LIB_DEFAULT_STYLE) must be
    // kept so implementation units can use it.
    if (is_include_guard_define(llvm::StringRef(buf, hashOff + 1), hashOff,
                                macro_name))
      return;

    macros.push_back({std::move(macro_name), std::move(body), hashOff, endOff});
    macros.back().cond_depth = cond_depth;
  }

  void MacroUndefined(const clang::Token &name,
                      const clang::MacroDefinition &,
                      const clang::MacroDirective *) override {
    if (!sm.isInMainFile(name.getLocation())) return;
    auto id = name.getIdentifierInfo();
    if (!id) return;
    // NOTE: StringRef must be converted with .str() — std::format treats a raw
    // StringRef as a range of chars (`['P','R',...]`) and never matches.
    for (auto &m : macros) {
      if (!m.body.starts_with(std::format("#define {}", id->getName().str())))
        continue;
      // An `#undef` nested deeper than the `#define` retracts the macro only
      // where its branch is taken — the `#undef X` + redefine under
      // `#if defined(__clang__)` idiom. The definition is still the one in
      // effect everywhere else, so it stays public; treating it as retracted
      // leaves the macro undefined on every other compiler.
      if (cond_depth > m.cond_depth) continue;
      m.undefed = true;
    }
  }

  void If(clang::SourceLocation loc, clang::SourceRange,
          ConditionValueKind) override { enter(loc); }
  void Ifdef(clang::SourceLocation loc, const clang::Token &,
             const clang::MacroDefinition &) override { enter(loc); }
  void Ifndef(clang::SourceLocation loc, const clang::Token &,
              const clang::MacroDefinition &) override { enter(loc); }
  void Endif(clang::SourceLocation loc, clang::SourceLocation) override {
    if (sm.isInMainFile(loc) && cond_depth > 0) --cond_depth;
  }

private:
  void enter(clang::SourceLocation loc) {
    if (sm.isInMainFile(loc)) ++cond_depth;
  }

  const clang::SourceManager &sm;
  std::vector<MacroRec> &macros;
  // Nesting of the `#if` blocks open at the current point. An include guard
  // counts as one, which is why definitions and undefs are compared to each
  // other rather than against zero.
  unsigned cond_depth = 0;
};

// Textually extract ALL #define lines from the original header, including
// those inside inactive #if blocks (macros are needed so that guard macros in
// the GMF resolve correctly on any platform). Macros inside a dead `#if 0`
// block are excluded: they are inactive on every platform and could carry
// stale values. When a macro is defined inside a conditional (`#if ... / #else
// / #endif`), the WHOLE conditional block is preserved verbatim so the correct
// branch expands on the actual platform (e.g. LIB_DISABLE_WARNINGS_PUSH_
// has different bodies for MSVC vs. others).
// Strip EVERY include from a macro-only conditional block except includes of
// generated macro/export headers (the macro chain): library-header includes are
// modules now (their macros are chained separately) so a raw textual include
// would pull the original header's declarations into the global module fragment
// and clash with the imported module (mylib-port.h → `#include
// "mylib/internal/mylib-port.h"`); system includes (`<iostream>`) must be
// stripped too because the module's GMF already carries those declarations and
// a consumer that imports the module AND textually includes the system header
// gets double-definition errors (mylib-port_macros.h → `#include <iostream>`);
// custom (`/custom/`) includes as well, since consumers reach custom content
// through the module, not the macro header.
std::string strip_block_includes(std::string full) {
  std::string cleaned;
  std::size_t lpos = 0;
  while (lpos < full.size()) {
    auto lnl = full.find('\n', lpos);
    if (lnl == std::string::npos) lnl = full.size();
    auto line = std::string_view(full).substr(lpos, lnl - lpos);
    auto ld = parse_directive(line, /*skip_hash_ws=*/true,
                              /*keyword_ends_crlf=*/true);
    bool keep = true;
    if (ld && ld->keyword == "include") {
      auto inc = ld->after;
      auto is = inc.find_first_not_of(" \t");
      bool is_macro_chain = false;
      if (is != std::string_view::npos && inc[is] == '"') {
        auto path = inc.substr(is + 1);
        auto quote = path.find('"');
        if (quote != std::string_view::npos) path = path.substr(0, quote);
        is_macro_chain = path.ends_with("_macros.h") || path.ends_with("_export.h");
      }
      if (!is_macro_chain) keep = false;
    }
    if (keep) {
      cleaned += full.substr(lpos, lnl - lpos + 1);
    } else {
      // Drop the include line but keep the newline structure.
      cleaned += '\n';
    }
    lpos = lnl + 1;
  }
  return cleaned;
}

export void extract_textual_macros(
    const std::string &original, std::vector<MacroRec> &macros,
    // Conditionals that choose between arms and are NOT emitted verbatim.
    // What is defined inside one belongs to the arm that selects it, and no
    // emitted block carries that arm along, so neither the header nor the
    // macros file may take the definition out on its own.
    std::vector<std::pair<unsigned, unsigned>> *unemitted_arm_ranges = nullptr,
    // Blocks that raise a diagnostic about a macro this file itself defines.
    // They are cut out of the header body; see the loop that fills them.
    std::vector<std::pair<unsigned, unsigned>> *rejection_ranges = nullptr) {
  std::string_view src(original);
  std::size_t pos = 0;
  std::set<std::string> seen_macro_names;
  for (auto &m : macros)
    seen_macro_names.insert(m.name);
  // Every non-guard name this file defines, wherever it sits, and the blocks
  // that raise a diagnostic without defining anything.
  std::set<std::string, std::less<>> defined_names;
  std::vector<std::pair<std::size_t, std::size_t>> rejections;

  struct CondBlock {
    std::size_t start;  // offset of the `#if` line
    bool is_zero;       // `#if 0` (dead on every platform)
    bool has_define;    // contains a non-guard #define (in any nested branch)
    bool has_code;      // contains a non-preprocessor (declaration) line
    bool has_error;     // contains `#error`/`#warning` and nothing else
    // Has `#elif`/`#else` arms, so what is inside it belongs to one arm and
    // not to the block as a whole.
    bool has_arms = false;
    // Conditionals closed while this one was open, as indices into `cands`.
    std::vector<std::size_t> children;
  };
  // A closed conditional and whether it is worth emitting verbatim. Whether a
  // NESTED one also needs emitting cannot be decided when it closes: that turns
  // on whether the enclosing block ends up emitted, and the enclosing block's
  // code may all come after it — which is exactly the shape of a header whose
  // include guard wraps the whole file. Deciding early suppressed a nested
  // conditional as "the parent covers it" when the parent was later disqualified
  // by the code below it and never emitted at all, losing the guard.
  struct Cand {
    std::size_t start, end;
    bool worth;
    bool has_arms = false;
    std::vector<std::size_t> children;
  };
  std::vector<Cand> cands;
  std::vector<std::size_t> roots;
  std::vector<CondBlock> blocks;
  bool in_macro_cont = false;  // inside a multi-line `#define \` continuation

  auto is_guard_name = [](std::string_view mname) {
    return looks_like_guard_name(mname);
  };
  auto capture_macro = [&](std::string_view line, std::size_t hash_pos,
                           std::size_t nl) {
    auto define = std::string(line.substr(hash_pos));
    auto ns = define.find_first_of(" \t", 7);  // skip "#define"
    if (ns == std::string_view::npos) return;
    auto rest = define.substr(ns + 1);
    auto name_start = rest.find_first_not_of(" \t");
    if (name_start == std::string_view::npos) return;
    auto name_end = rest.find_first_of("( \t", name_start);
    auto mname = rest.substr(name_start,
                 name_end == std::string_view::npos
                     ? std::string_view::npos
                     : name_end - name_start);
    if (seen_macro_names.count(std::string(mname))) return;
    // Capture the full macro body, consuming `\` continuation lines.
    auto full = define + "\n";
    auto cur = nl + 1;
    while (full.ends_with("\\\n") && cur < src.size()) {
      auto next_nl = src.find('\n', cur);
      if (next_nl == std::string_view::npos) next_nl = src.size();
      full += std::string(src.substr(cur, next_nl - cur)) + "\n";
      cur = next_nl + 1;
    }
    bool is_guard = is_guard_name(mname);
    if (!is_guard)
      macros.push_back({std::string(mname), std::move(full),
                        (unsigned)hash_pos, (unsigned)cur});
    seen_macro_names.insert(std::string(mname));
  };

  while (pos < src.size()) {
    auto nl = src.find('\n', pos);
    if (nl == std::string_view::npos) break;
    auto line = src.substr(pos, nl - pos);
    auto d = parse_directive(line, /*skip_hash_ws=*/true,
                             /*keyword_ends_crlf=*/true);
    if (d) {
      auto &dir = d->keyword;
      if (dir == "ifdef" || dir == "ifndef" || dir == "if") {
        bool is_zero = false;
        if (dir == "if") {
          auto rest = d->after;
          auto ns = rest.find_first_not_of(" \t");
          is_zero = ns != std::string_view::npos && rest[ns] == '0';
        }
        blocks.push_back({pos, is_zero, false, false, false, false, {}});
      } else if (dir == "endif") {
        if (!blocks.empty()) {
          auto b = std::move(blocks.back());
          blocks.pop_back();
          // A macro-only conditional is worth emitting verbatim so the correct
          // branch expands on the actual platform. A block mixing macros with
          // code is not (it would declare entities in the global module
          // fragment); its macros are still captured individually by
          // PPCallbacks. Whether a nested one is redundant — covered by an
          // enclosing block that gets emitted — is settled after the scan.
          bool worth = b.has_define && !b.has_code && !b.is_zero;
          if (unemitted_arm_ranges && b.has_arms && !worth)
            unemitted_arm_ranges->push_back(
                {(unsigned)b.start, (unsigned)(nl + 1)});
          if (rejection_ranges && b.has_error && !b.has_define &&
              !b.has_code && !b.has_arms && !b.is_zero)
            rejections.push_back({b.start, nl + 1});
          cands.push_back({b.start, nl + 1, worth, b.has_arms,
                           std::move(b.children)});
          auto idx = cands.size() - 1;
          if (!blocks.empty())
            blocks.back().children.push_back(idx);
          else
            roots.push_back(idx);
        }
      } else if (dir == "error" || dir == "warning") {
        // A diagnostic the library raises about its own configuration. Which
        // macros it is judging is settled after the scan: the ones it rejects
        // are usually defined below it.
        if (!blocks.empty()) blocks.back().has_error = true;
      } else if (dir == "else" || dir == "elif") {
        // keep blocks unchanged, but remember that this one chooses between
        // arms: what is nested inside belongs to the arm it sits in, which is
        // not something recorded here.
        if (!blocks.empty()) blocks.back().has_arms = true;
      } else if (dir == "define" && !d->after.empty()) {
        {
          auto after = d->after;
          auto ns = after.find_first_not_of(" \t");
          if (ns != std::string_view::npos) {
            auto ne = after.find_first_of("( \t", ns);
            auto mname = after.substr(
                ns, ne == std::string_view::npos ? ne : ne - ns);
            if (!is_guard_name(mname)) defined_names.emplace(mname);
          }
        }
        if (blocks.empty()) {
          capture_macro(line, d->hash_pos, nl);
        } else {
          // Mark every enclosing conditional as containing a define so the
          // outermost one is emitted verbatim with its guards, even if the
          // macro was already captured by PPCallbacks from the active branch.
          auto after = d->after;
          auto name_start = after.find_first_not_of(" \t");
          if (name_start != std::string_view::npos) {
            auto name_end = after.find_first_of("( \t", name_start);
            auto mname = after.substr(name_start,
                         name_end == std::string_view::npos
                             ? std::string_view::npos
                             : name_end - name_start);
            if (!is_guard_name(mname))
              for (auto &b : blocks) b.has_define = true;
          }
        }
      }
    } else if (!blocks.empty() && !in_macro_cont) {
      // A non-preprocessor line inside a conditional (declaration, `using`,
      // `namespace`, ...). A block mixing macros with code must not be emitted
      // verbatim (it would declare entities in the global module fragment).
      auto ns = line.find_first_not_of(" \t");
      bool blank = ns == std::string_view::npos;
      bool comment = ns != std::string_view::npos && line[ns] == '/';
      if (!blank && !comment)
        for (auto &b : blocks) b.has_code = true;
    }
    // Track multi-line directive continuation (`#define \`, `#if \`): the
    // continuation lines are part of the directive, never code, so a line like
    // `!defined(__EXCEPTIONS)` under an `#if ... && \` must not mark the
    // enclosing conditional as containing code (which would suppress emitting
    // its guard block verbatim).
    bool line_ends_cont = !line.empty() && line.back() == '\\';
    if (d && line_ends_cont) {
      in_macro_cont = true;
    } else if (in_macro_cont) {
      if (!line_ends_cont) in_macro_cont = false;
    }
    pos = nl + 1;
  }

  // Emit the outermost conditional on each path that is worth emitting; an
  // emitted block carries its nested ones along, so descend only past blocks
  // that are not emitted themselves. Outermost-first keeps the emitted blocks
  // in source order.
  // A header whose guarded body is nothing but directives leaves its own
  // include guard eligible for verbatim emission -- there is no code inside it
  // to disqualify it. Emitted whole, the guard's `#define` goes to the macros
  // file, and the header reads that file ABOVE its guard: the guard is already
  // defined, the body never runs, and what it was there to do never happens.
  //
  //   cstdio_test.cpp:86: error: no type named 'utf8_codecvt_facet' in
  //     namespace 'boost::filesystem::detail'
  //
  // The macros file carries `#pragma once`; it has no use for a guard of its
  // own, so the guard's three lines come out and what they held stays.
  auto strip_include_guard = [&](std::string block) {
    std::vector<std::string_view> lines;
    std::string_view v(block);
    for (std::size_t p = 0; p <= v.size();) {
      auto nl = v.find('\n', p);
      if (nl == std::string_view::npos) {
        if (p < v.size()) lines.push_back(v.substr(p));
        break;
      }
      lines.push_back(v.substr(p, nl - p));
      p = nl + 1;
    }
    if (lines.empty()) return block;
    auto d = parse_directive(lines[0], true, true);
    if (!d || d->keyword != "ifndef") return block;
    auto after = d->after;
    auto ns = after.find_first_not_of(" \t");
    if (ns == std::string_view::npos) return block;
    auto ne = after.find_first_of(" \t", ns);
    auto name = after.substr(ns, ne == std::string_view::npos ? ne : ne - ns);
    if (!is_guard_name(name)) return block;
    // Its `#define`, and the `#endif` that closes the block.
    std::size_t def_at = lines.size(), endif_at = lines.size();
    for (std::size_t i = 1; i < lines.size(); ++i) {
      auto ld = parse_directive(lines[i], true, true);
      if (!ld) continue;
      if (def_at == lines.size() && ld->keyword == "define") {
        auto a = ld->after;
        auto s0 = a.find_first_not_of(" \t");
        if (s0 == std::string_view::npos) continue;
        auto e0 = a.find_first_of("( \t", s0);
        if (a.substr(s0, e0 == std::string_view::npos ? e0 : e0 - s0) == name)
          def_at = i;
      }
      if (ld->keyword == "endif") endif_at = i;
    }
    if (def_at == lines.size()) return block;
    std::string out;
    for (std::size_t i = 1; i < lines.size(); ++i) {
      if (i == def_at || i == endif_at) continue;
      out += lines[i];
      out += '\n';
    }
    return out;
  };
  auto emit = [&](auto &&self, std::size_t idx) -> void {
    auto &c = cands[idx];
    if (c.worth) {
      auto full = strip_include_guard(
          std::string(src.substr(c.start, c.end - c.start)));
      macros.push_back({"", strip_block_includes(std::move(full)),
                        (unsigned)c.start, (unsigned)c.end});
      return;
    }
    // An unemitted block with arms takes what is inside it down too. A nested
    // block means
    // what its enclosing one says it means: emitted on its own it is read by
    // every build, including the ones the enclosing condition was there to
    // exclude —
    //
    //   #if defined(__clang__)
    //   ...
    //   #elif defined(_MSC_VER)
    //   #  if _MSC_FULL_VER < 190024210
    //   #    undef LIB_CONSTEXPR
    //   #    define LIB_CONSTEXPR      // nothing, for a compiler this is not
    //   #  endif
    //   #endif
    //
    // and `_MSC_FULL_VER` is undefined elsewhere, which the preprocessor reads
    // as 0 and finds true. Boost.Mp11 loses every constexpr that way.
    //
    // The arm cannot be carried along instead: the enclosing record spans the
    // whole chain from `#if` to `#endif`, and which arm a nested block sits in
    // is not something the scan keeps. What the active branch defines is
    // captured macro by macro regardless, so what goes unemitted here is only
    // what another platform would have defined — which is the part that must
    // not be written unconditionally.
    //
    // Without arms there is nothing to be in the wrong one of, and descending
    // is what a header whose include guard wraps the whole file needs: that
    // guard holds code by definition, and everything real is nested in it.
    if (c.has_arms) return;
    // Siblings are recorded as they close, which for siblings is source order —
    // and source order is what has to be kept, since a macro used in a later
    // conditional's `#if` must already be defined by an earlier one.
    for (auto child : c.children) self(self, child);
  };
  for (auto idx : roots) emit(emit, idx);

  // A header that rejects one of its own macros as user input reads its
  // generated macros file above that rejection, so what the rejection finds is
  // the library's own definition and it fires on the first read:
  //
  //   config.hpp:76: error: BOOST_FILESYSTEM_WINDOWS_API and
  //     BOOST_FILESYSTEM_POSIX_API must not be defined by users
  //
  // Once the macros are the library's to replay, the header can no longer tell
  // a user's definition from its own, and the question the rejection asks has
  // no answer left. Only the ones judging a macro this file defines go: a
  // diagnostic about anything else is still about something the header can see.
  for (auto [start, end] : rejections) {
    auto nl = src.find('\n', start);
    auto cond = src.substr(start, nl == std::string_view::npos ? nl
                                                               : nl - start);
    for (std::size_t i = 0; i < cond.size();) {
      if (!(std::isalpha((unsigned char)cond[i]) || cond[i] == '_')) {
        ++i;
        continue;
      }
      auto j = i;
      while (j < cond.size() &&
             (std::isalnum((unsigned char)cond[j]) || cond[j] == '_'))
        ++j;
      if (defined_names.count(cond.substr(i, j - i))) {
        rejection_ranges->push_back({(unsigned)start, (unsigned)end});
        break;
      }
      i = j;
    }
  }
}

// Result of override-guard analysis for a macro define.
export struct OverrideGuardRange {
  bool active = false;
  unsigned guard_start = 0;  // offset of the `#` of the `#if` guard line
  unsigned endif_start = 0;  // offset of the `#` of the matching `#endif`
};

// Detects an override guard `#if !defined(NAME)` / `#ifndef NAME` directly
// above (only blank lines between) the define at `hash_off`, whose block ALSO
// contains at least one non-preprocessor code line. Such a block mixes a macro
// definition with inline code (a `#if !defined(LIB_LOG_)` guard gates
// both `#define LIB_LOG_` and the `log_to_stderr`/`flush_info_log` inline
// helpers). When the macro is moved to the macros file the guard must be
// dropped too — otherwise the guard re-evaluates with the macro already
// defined and the inline helpers are lost.
export OverrideGuardRange override_guard_with_code(const std::string &src,
                                                   unsigned hash_off,
                                                   const std::string &name) {
  OverrideGuardRange out;
  auto nl = src.rfind('\n', hash_off - 1);
  std::size_t define_line = nl == std::string_view::npos ? 0 : nl + 1;
  std::size_t guard_line = define_line;
  for (;;) {
    auto prev = prev_line_start(src, guard_line);
    if (prev >= guard_line) break;
    std::string_view line(src.data() + prev, guard_line - prev - 1);
    auto ns = line.find_first_not_of(" \t\r\n");
    guard_line = prev;
    if (ns != std::string_view::npos) break;  // first non-blank line above
  }
  std::string_view gline(src.data() + guard_line);
  auto gnl = gline.find('\n');
  std::string_view gl = gnl == std::string_view::npos ? gline : gline.substr(0, gnl);
  auto d = parse_directive(gl, /*skip_hash_ws=*/true,
                           /*keyword_ends_crlf=*/true);
  if (!d) return out;
  std::string cond;
  if (d->keyword == "ifndef") {
    cond = d->after;
  } else if (d->keyword == "if") {
    auto after = d->after;
    auto ns = after.find_first_not_of(" \t");
    if (ns == std::string_view::npos ||
        !after.substr(ns).starts_with("!defined("))
      return out;
    cond = after.substr(ns + 9);
    auto cp = cond.find(')');
    if (cp != std::string_view::npos) cond = cond.substr(0, cp);
  } else {
    return out;
  }
  auto ns2 = cond.find_first_not_of(" \t");
  if (ns2 != std::string_view::npos) cond = cond.substr(ns2);
  if (cond != name) return out;
  // Scan forward from the define to the matching `#endif`, checking whether the
  // block contains real code (non-preprocessor, non-comment, non-continuation).
  std::size_t i = hash_off;
  int depth = 0;
  bool found_code = false;
  unsigned endif_start = 0;
  while (i < src.size()) {
    auto eol = src.find('\n', i);
    if (eol == std::string_view::npos) break;
    std::string_view line(src.data() + i, eol - i);
    auto d2 = parse_directive(line, /*skip_hash_ws=*/true,
                              /*keyword_ends_crlf=*/true);
    if (d2) {
      auto &k = d2->keyword;
      if (k == "if" || k == "ifdef" || k == "ifndef") ++depth;
      else if (k == "endif") {
        if (depth == 0) { endif_start = (unsigned)i; break; }
        --depth;
      }
    } else {
      auto ns3 = line.find_first_not_of(" \t\r\n");
      bool blank = ns3 == std::string_view::npos;
      bool comment = !blank && line[ns3] == '/';
      if (!blank && !comment && !line.ends_with("\\")) found_code = true;
    }
    i = eol + 1;
  }
  if (endif_start && found_code) {
    out.active = true;
    out.guard_start = (unsigned)guard_line;
    out.endif_start = endif_start;
  }
  return out;
}

// Build the generated macros file (`<stem>_macros.h`): every moved public macro
// plus its doc comment, chained dependency macros files, and export-marker
// insertions applied to macro bodies that declare library entities (a
// declaration macro). Returns an empty string when there is nothing to emit.
// Whether the header protects itself against being included twice. One that
// does not is meant to be re-read — Boost's `assert.hpp` recomputes BOOST_ASSERT
// from whatever is defined at each inclusion — and its macros file must be
// re-read with it, or the second include silently keeps the first answer.
// The `#undef X` lines a header writes BEFORE it defines X — the "start from
// nothing, then decide" opening of a header meant to be re-read. They belong
// with the definitions they precede: left behind while the definitions move to
// the macros file, they undo the file the header just included.
//
// An `#undef` that comes AFTER a definition is a different idiom (retract and
// redefine under a condition) and is handled where the definition is collected.
export std::vector<std::pair<unsigned, std::string>> leading_undefs(
    const std::string &src, const std::vector<MacroRec> &macros,
    // Names defined only INSIDE a reproduced conditional block. They have no
    // record of their own, so their resets are found by name; the ones written
    // inside a block travel with it and are skipped here.
    const std::set<std::string> &block_names = {},
    const std::vector<std::pair<unsigned, unsigned>> &block_ranges = {}) {
  std::vector<std::pair<unsigned, std::string>> out;
  auto in_a_block = [&](std::size_t pos) {
    for (auto &[start, end] : block_ranges)
      if (pos >= start && pos < end) return true;
    return false;
  };
  auto line_starts_here = [&](std::size_t pos, std::size_t len) {
    auto after = pos + len;
    if (after < src.size() && is_ident_char(src[after])) return false;
    auto ls = src.rfind('\n', pos);
    return src.find_first_not_of(" \t", ls == std::string::npos ? 0 : ls + 1) == pos;
  };
  for (const auto &name : block_names) {
    auto needle = std::format("#undef {}", name);
    for (std::size_t pos = src.find(needle); pos != std::string::npos;
         pos = src.find(needle, pos + 1))
      if (!in_a_block(pos) && line_starts_here(pos, needle.size()))
        out.emplace_back(static_cast<unsigned>(pos), name);
  }
  for (const auto &m : macros) {
    if (m.name.empty()) continue;
    auto needle = std::format("#undef {}", m.name);
    for (std::size_t pos = src.find(needle); pos != std::string::npos;
         pos = src.find(needle, pos + 1)) {
      if (pos >= m.start_off) break;  // after the definition: not this idiom
      auto after = pos + needle.size();
      if (after < src.size() && is_ident_char(src[after])) continue;
      auto ls = src.rfind('\n', pos);
      if (src.find_first_not_of(" \t", ls == std::string::npos ? 0 : ls + 1) != pos)
        continue;
      out.emplace_back(static_cast<unsigned>(pos), m.name);
    }
  }
  return out;
}

export bool header_guards_itself(const std::string &src) {
  if (src.contains("#pragma once")) return true;
  std::size_t pos = 0;
  std::string pending;
  while (pos < src.size()) {
    auto nl = src.find('\n', pos);
    if (nl == std::string::npos) nl = src.size();
    auto line = std::string_view(src).substr(pos, nl - pos);
    pos = nl + 1;
    auto d = parse_directive(line);
    if (!d) continue;
    if (pending.empty()) {
      if (d->keyword != "ifndef") return false;  // some other directive first
      auto e = d->after.find_first_of(" \t\r");
      pending = e == std::string::npos ? d->after : d->after.substr(0, e);
      continue;
    }
    if (d->keyword != "define") return false;
    auto e = d->after.find_first_of(" \t\r");
    return (e == std::string::npos ? d->after : d->after.substr(0, e)) == pending;
  }
  return false;
}


// The preprocessor lines of a conditional, and nothing else. A chain that
// chooses between arms and also declares something cannot be emitted whole --
// the declarations would land in the global module fragment -- but its macros
// are meaningless without the arms that select them, and a module unit that
// imports the header rather than reading it has only this file to learn them
// from. What defines a macro is a directive, so dropping every other line
// keeps the machine and leaves the declarations behind.
std::string directives_only(std::string_view block) {
  std::string out;
  bool cont = false;
  std::size_t pos = 0;
  while (pos < block.size()) {
    auto nl = block.find('\n', pos);
    if (nl == std::string_view::npos) nl = block.size();
    auto line = block.substr(pos, nl - pos);
    bool keep = cont;
    if (!keep) {
      auto d = parse_directive(line, /*skip_hash_ws=*/true,
                              /*keyword_ends_crlf=*/true);
      // An include would bring the declarations back by another door.
      keep = d && d->keyword != "include";
    }
    if (keep) {
      out += line;
      out += '\n';
      cont = !line.empty() && line.back() == '\\';
    }
    pos = nl + 1;
  }
  return out;
}

export std::string build_macros_file(
    const std::vector<MacroRec> &export_macros,
    // Macro header -> the conditional context it must be chained in under.
    const std::map<std::string, std::vector<std::string>> &extra_macro_includes,
    // Include lines the reproduced conditional blocks need in order to be
    // evaluated here. See the caller.
    const std::vector<std::string> &block_support_includes,
    const std::set<std::string> &block_defined_names,
    const std::vector<std::pair<unsigned, unsigned>> &block_ranges,
    const std::string &original,
    const std::vector<ModPoint> &mods,
    const std::string &export_include,
    // See header_guards_itself: a re-includable header's macros file must be
    // re-read too — and it is the only copy of that header's conditional
    // machine, so the includes inside those branches belong to it as well.
    // (For a header included once, the branch is reproduced here AND kept in
    // the body, and the includes stay with the body.)
    bool guard_once = true,
    // See extract_textual_macros: a definition inside an arm nobody carries
    // stays where its arm selects it, which is the header.
    const std::vector<std::pair<unsigned, unsigned>> &unemitted_arm_ranges =
        {}) {
  const bool sole_copy = !guard_once;
  std::string mout;
  if (guard_once) mout += "#pragma once\n\n";
  // See leading_undefs: the header's own "start from nothing" undefs come
  // first, so re-reading this file recomputes rather than collides.
  {
    std::set<std::string> seen;
    for (auto &[off, name] :
         leading_undefs(original, export_macros, block_defined_names,
                        block_ranges))
      if (seen.insert(name).second) mout += std::format("#undef {}\n", name);
    if (!seen.empty()) mout += "\n";
  }
  for (const auto &bi : block_support_includes) mout += bi;
  if (!block_support_includes.empty()) mout += "\n";
  for (const auto &[em, guards] : extra_macro_includes) {
    // The condition the include sat under travels with it: a header included
    // only in one branch defines its macros only in that branch, and chaining
    // them in unconditionally lets a definition the other branch never wanted
    // win.
    auto vg = valid_guards(guards);
    for (const auto &g : vg) mout += g;
    mout += std::format("#include \"{}\"\n", em);
    for (std::size_t i = 0; i < vg.size(); ++i) mout += "#endif\n";
  }
  if (!extra_macro_includes.empty()) mout += "\n";
  // Apply the export-marker insertions that fall inside a macro's definition
  // range to the moved macro text, so a macro whose body declares a library
  // entity (e.g. a declaration macro) still exports it wherever it expands.
  // Multiple invocations of the same macro (e.g. `DECLARE_X(IsA, ...)` and
  // `DECLARE_X(IsB, ...)`) produce markers at the SAME spelling locations
  // inside the shared body; identical insertions are applied once. Different
  // consumers may also route both a plain export marker and an `extern "C++"`
  // one to the same offset (a flag declared via a shared macro): prefer the
  // plain export, which is what the entity actually needs in its own module.
  auto apply_in_range_mods = [&](const std::string &text, unsigned base) {
    std::string out = text;
    std::vector<ModPoint> in_range;
    for (auto &m : mods) {
      if (m.len != 0) continue;  // insertions (export markers) only
      if (m.offset >= base && m.offset < base + text.size())
        in_range.push_back({m.offset - base, 0, m.text});
    }
    std::ranges::stable_sort(
        in_range, [](const ModPoint &a, const ModPoint &b) {
          if (a.offset != b.offset) return a.offset > b.offset;
          // Different consumers may route both a plain export marker and an
          // `extern "C++"` one to the same offset. Prefer the `extern "C++"`
          // variant: a cross-module shared entity needs global-module linkage
          // wherever the macro expands.
          bool a_ext = a.text.find("extern \"C++\"") != std::string::npos;
          bool b_ext = b.text.find("extern \"C++\"") != std::string::npos;
          return a_ext && !b_ext;
        });
    std::set<unsigned> inserted;
    for (auto &m : in_range) {
      if (!inserted.insert(m.offset).second) continue;
      out.insert(m.offset, m.text);
    }
    return out;
  };
  bool has_marked_macros = false;
  std::set<unsigned> emitted_arm_chains;
  for (auto &m : export_macros) {
    // Skip bare active definitions covered by an emitted conditional block:
    // one that covers every platform makes any definition of the name dead,
    // and a definition sitting inside a block travels with it either way —
    // emitting that one bare would strip its condition and hand every platform
    // whichever branch happened to be active during the conversion.
    if (!m.name.empty()) {
      if (block_defined_names.count(m.name)) continue;
      bool in_block = false;
      for (auto &[start, end] : block_ranges)
        if (m.start_off >= start && m.end_off <= end) { in_block = true; break; }
      if (in_block) continue;
      // And a definition inside an arm-bearing chain that is NOT emitted: no
      // block carries its condition, so written here it is written for every
      // compiler, saying what only the converting one had any right to say.
      // A definition inside an arm-bearing chain that is not emitted whole.
      // Written bare it speaks for every compiler; left out altogether it is
      // gone from the one file a module unit importing this header can learn
      // it from. So the chain arrives here as its directives alone, once, in
      // the place its first definition would have taken.
      bool in_arm = false;
      for (auto &[start, end] : unemitted_arm_ranges)
        if (m.start_off >= start && m.end_off <= end) {
          if (emitted_arm_chains.insert(start).second)
            mout += directives_only(
                std::string_view(original).substr(start, end - start));
          in_arm = true;
          break;
        }
      if (in_arm) continue;
    }
    // Move the doc-comment block that precedes the macro into the macros file
    // next to the macro it documents.
    if (!m.name.empty() && m.start_off != 0) {
      auto cstart = preceding_comment_start(original, m.start_off);
      if (cstart != 0 && cstart < m.start_off) {
        mout += std::string(original, cstart, m.start_off - cstart);
        if (mout.back() != '\n') mout += '\n';
      }
    }
    if (m.start_off != 0 && m.end_off > m.start_off) {
      // Apply export markers that fall inside this macro's definition range
      // (named macro) or inside a macro-only conditional block (name empty) to
      // the ORIGINAL slice — the marker offsets are in original-file space.
      // A conditional block's body has includes stripped, so re-apply that
      // stripping to the marked slice afterwards and compare against the
      // stripped body to detect whether any marker was actually applied.
      auto marked = apply_in_range_mods(
          std::string(original, m.start_off, m.end_off - m.start_off),
          m.start_off);
      if (m.name.empty()) {
        if (sole_copy) {
          // The branch's own includes come with it: nothing else will bring
          //   # include <assert.h>
          //   # define LIB_ASSERT(expr) assert(expr)
          // and the macro is expanded where the file is included.
          mout += std::move(marked);
          continue;
        }
        auto stripped = strip_block_includes(std::move(marked));
        if (stripped != m.body) {
          has_marked_macros = true;
          mout += std::move(stripped);
          continue;
        }
      } else if (marked != m.body) {
        has_marked_macros = true;
        mout += marked;
        continue;
      }
    }
    mout += m.body;
  }
  // A macro whose body carries an export marker needs the export macro
  // (TEST_LIB_EXPORT) defined when it is expanded. Emit the shared export
  // header into the chain so consumers and the module GMF can resolve it.
  if (has_marked_macros)
    mout.insert(std::string("#pragma once\n\n").size(), export_include);
  return mout;
}
