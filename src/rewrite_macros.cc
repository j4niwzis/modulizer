module;

export module modulizer.rewrite_macros;
import modulizer.astutil;
import modulizer.include_analysis;
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
    bool is_guard = false;
    {
      std::string_view before(buf, hashOff);
      auto nl = before.rfind('\n');
      if (nl != std::string_view::npos && nl > 0) {
        auto pnl = before.rfind('\n', nl - 1);
        auto start = pnl == std::string_view::npos ? 0 : pnl + 1;
        auto prev = before.substr(start, nl - start);
        is_guard = prev.starts_with("#ifndef") &&
                   prev.find(macro_name) != std::string_view::npos &&
                   looks_like_guard_name(macro_name);
      }
    }
    if (is_guard) return;

    macros.push_back({std::move(macro_name), std::move(body), hashOff, endOff});
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
      if (m.body.starts_with(std::format("#define {}", id->getName().str())))
        m.undefed = true;
    }
  }

private:
  const clang::SourceManager &sm;
  std::vector<MacroRec> &macros;
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
    auto ld = parse_directive(line, /*skip_hash_ws=*/false,
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

export void extract_textual_macros(const std::string &original,
                                   std::vector<MacroRec> &macros) {
  std::string_view src(original);
  std::size_t pos = 0;
  std::set<std::string> seen_macro_names;
  for (auto &m : macros)
    seen_macro_names.insert(m.name);

  struct CondBlock {
    std::size_t start;  // offset of the `#if` line
    bool is_zero;       // `#if 0` (dead on every platform)
    bool has_define;    // contains a non-guard #define (in any nested branch)
    bool has_code;      // contains a non-preprocessor (declaration) line
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
    auto d = parse_directive(line, /*skip_hash_ws=*/false,
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
        blocks.push_back({pos, is_zero, false, false, {}});
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
          cands.push_back({b.start, nl + 1,
                           b.has_define && !b.has_code && !b.is_zero,
                           std::move(b.children)});
          auto idx = cands.size() - 1;
          if (!blocks.empty())
            blocks.back().children.push_back(idx);
          else
            roots.push_back(idx);
        }
      } else if (dir == "else" || dir == "elif") {
        // keep blocks unchanged
      } else if (dir == "define" && !d->after.empty()) {
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
  auto emit = [&](auto &&self, std::size_t idx) -> void {
    auto &c = cands[idx];
    if (c.worth) {
      auto full = std::string(src.substr(c.start, c.end - c.start));
      macros.push_back({"", strip_block_includes(std::move(full)),
                        (unsigned)c.start, (unsigned)c.end});
      return;
    }
    // `children` is innermost-closed-first; source order is the reverse.
    for (auto it = c.children.rbegin(); it != c.children.rend(); ++it)
      self(self, *it);
  };
  for (auto idx : roots) emit(emit, idx);
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
  auto d = parse_directive(gl, /*skip_hash_ws=*/false,
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
    auto d2 = parse_directive(line, /*skip_hash_ws=*/false,
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
export std::string build_macros_file(
    const std::vector<MacroRec> &export_macros,
    const std::set<std::string> &extra_macro_includes,
    const std::set<std::string> &block_defined_names,
    const std::string &original,
    const std::vector<ModPoint> &mods,
    const std::string &export_include) {
  std::string mout;
  mout += "#pragma once\n\n";
  for (const auto &em : extra_macro_includes)
    mout += std::format("#include \"{}\"\n", em);
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
  for (auto &m : export_macros) {
    // Skip bare active definitions covered by an emitted conditional block.
    if (!m.name.empty() && block_defined_names.count(m.name)) continue;
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
