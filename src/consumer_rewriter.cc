module;

export module modulizer.consumer_rewriter;
import modulizer.include_analysis;
import modulizer.naming;
import modulizer.util;
import libtooling;
import std;

namespace {

// The block of standard headers a consumer needs once the library headers no
// longer pull them in textually (module interface units keep them in the GMF).
// Emitted when --import-std is NOT set; --import-std replaces it with
// `import std;`. Matches the includes consumer code previously received
// transitively from the library headers.
// C headers whose DECLARATIONS the std module also provides. Textually in a
// header, they are re-read after an importing translation unit has already
// imported std, and the inline overloads (memchr, struct tm, ...) collide.
const std::set<std::string> kStdProvidedCHeaders = {
    "string.h", "time.h", "stdio.h", "stdlib.h", "stddef.h",
    "stdint.h", "wchar.h", "ctype.h", "math.h", "wctype.h",
};

const std::vector<std::string> kStdIncludes = {
    "algorithm", "cstdio", "cstdlib", "cstring", "exception", "functional",
    "iostream", "iterator", "limits", "map", "memory", "ostream", "set",
    "sstream", "string", "tuple", "type_traits", "utility", "vector",
};

} // namespace

// Where a header's macros file sits when the include map does not say. The
// conversion writes one whenever a header defines macros, so a lookup that
// came back empty is worth asking the compiler about rather than treating as
// an answer: a path that resolves differently at rewrite time than at build
// time otherwise costs the consumer every macro it used to get textually.
std::string conventional_macros_header(const std::string &include_path,
                                       bool hyphen_macros) {
  return replacement_macros_header(include_path, hyphen_macros);
}

export struct ConsumerHeaderInfo {
  std::string module;
  std::string macro_header;  // empty when the header has no generated macro file
  // The header does not guard itself against being included twice, so WHERE it
  // is included matters: Boost's `assert.hpp` recomputes BOOST_ASSERT from
  // whatever is defined at each inclusion, and a test includes it eight times
  // with different macros set between. Such an include is replaced where it
  // stands rather than hoisted into the block at the top.
  bool reincludable = false;
};

export struct ConsumerRewriteOptions {
  bool import_std = false;
  // This source is a HEADER other consumers include. It must not keep the C
  // headers `import std` already provides: an including translation unit has
  // imported by the time it reaches them, and the declarations collide —
  //   error: redefinition of 'void* memchr(void*, int, size_t)'
  bool is_header = false;
  // How this library names its files. A hyphenated library gets hyphenated
  // macros files, and asking for the underscored name asks for a file nobody
  // wrote — taking with it every macro the consumer came for.
  bool hyphen_macros = false;
  std::map<std::string, ConsumerHeaderInfo> include_to_module;
  // System C/POSIX headers the consumer uses macros/symbols from (traced from
  // its source via trace_consumer_system_includes). `import std.compat` cannot
  // provide the C library's macros (errno, assert, INT_MAX, NULL, stderr) or
  // POSIX declarations/types (ssize_t, pthread_*), so these includes must be
  // re-added once the library headers no longer pull them in textually.
  std::set<std::string> required_system_includes;
  // Headers the library reached for that some module also provides. Whether
  // the module actually provides them is a build-time choice, so both answers
  // are emitted: the import where the provider's guard is on, the include
  // where it is not. Without the second the consumer gets neither — the
  // library's own units include it in their global module fragment, which is
  // pruned to what is decl-reachable and drops anything found only by ADL.
  std::vector<ModuleReplacement> required_module_includes;
  // Modules the consumer references internal entities of (traced from the
  // consumer's source against the library headers). Those entities are
  // exported from their defining sub-modules but not re-exported by the
  // wrapper (internal-consumer reachability), so the consumer must import the
  // owning modules itself.
  std::vector<std::string> traced_imports;
  // Non-empty switches on dual mode: the includes this rewrite replaces are
  // kept in an `#else` branch, so the file still compiles as a plain
  // translation unit against the same headers. A test that only ever imports
  // proves the module build and nothing about the classic one.
  std::string dual_macro;
  // The modules that provide headers this consumer includes. Their includes
  // become imports too, behind each provider's own guard: a consumer that
  // keeps including a provider textually while the library it uses imports the
  // same provider ends up with two of it —
  //
  //   error: declaration of 'mp_bool' in the global module follows declaration
  //          in module boost.mp11.integral
  ModuleReplacements module_replacements;
};

// Convert a consumer source file to use module imports: `#include
// "lib/header.h"` lines become `import lib.module;` plus the corresponding
// generated macro-header include (macros do not cross module boundaries), and
// standard-library headers are either covered by `import std;` (--import-std)
// or emitted as an explicit include block. The transform is idempotent.
export std::string rewrite_consumer_source(
    const std::string &src, const ConsumerRewriteOptions &options) {
  auto line_at = [&](std::size_t pos, std::size_t &nl) {
    nl = src.find('\n', pos);
    if (nl == std::string::npos) nl = src.size();
    return std::string_view(src).substr(pos, nl - pos);
  };

  // Pass 1: existing imports/includes, so the block stays idempotent.
  std::set<std::string> existing_imports;
  std::set<std::string> existing_std_includes;
  std::set<std::string> existing_quoted_includes;
  {
    std::size_t pos = 0;
    while (pos < src.size()) {
      std::size_t nl = 0;
      auto line = line_at(pos, nl);
      auto ns = line.find_first_not_of(" \t");
      if (ns != std::string_view::npos &&
          line.substr(ns).starts_with("import ")) {
        auto rest = line.substr(ns + 7);
        auto semi = rest.find(';');
        if (semi != std::string_view::npos) {
          auto mod = rest.substr(0, semi);
          auto e = mod.find_last_not_of(" \t");
          if (e != std::string_view::npos)
            existing_imports.insert(std::string(mod.substr(0, e + 1)));
        }
      }
      if (auto inc = parse_include_line(line)) {
        if (inc->is_quoted)
          existing_quoted_includes.insert(inc->path);
        else
          existing_std_includes.insert(inc->path);
      }
      pos = nl + 1;
      if (pos >= src.size()) break;
    }
  }



  // Pass 2: rewrite the lines.
  //
  // A library include can sit inside a preprocessor conditional (a
  // platform-guarded header). Such an include is replaced in place, so its
  // import keeps the guard that decided whether the header was included at
  // all. Everything that is needed regardless of any guard — the std import
  // (or the standard include block), traced imports and re-added C headers —
  // plus the imports of file-scope includes go into a single block anchored at
  // a position no conditional governs, so a build where a guard is false does
  // not lose them.
  std::vector<std::string> pending_imports;
  std::vector<std::string> pending_macros;
  // Those among them that are a guess at the conventional name rather than a
  // file the include map pointed at; they are asked for, not assumed.
  std::set<std::string> probed_macros;
  // The library includes this rewrite takes out, kept for dual mode's `#else`.
  std::vector<std::string> replaced_includes;
  std::vector<std::string> out;
  std::optional<std::size_t> insert_at;
  // The source line the block will sit above. Rewriting moves everything below
  // it, and a test that reports where it is — BOOST_CURRENT_LOCATION, __LINE__
  // — then reports the wrong place, so a `#line` puts the numbering back.
  std::size_t insert_at_src_line = 1;
  // Where each system include the consumer already writes ends up in `out`.
  // An include that survives BELOW the block does not spare the block from
  // emitting it: after the imports is exactly where it must not be.
  std::map<std::string, std::size_t> system_include_at;
  // System includes lifted out of the body because the block sits above them.
  std::vector<std::string> hoisted_system_includes;
  // Includes the source wrote that were turned into the guarded pair where
  // they stood. The block below leaves those alone; anything else a module
  // provides is still the block's to hand over, since the source never asked
  // for it and there is no pair in place to do it.
  std::set<std::string> replaced_in_source;
  {
    std::size_t pos = 0;
    std::size_t src_line = 1;
    // Depth of open #if/#ifdef/#ifndef blocks, and where the outermost
    // currently-open one started in `out` (the last unconditional position).
    int cond_depth = 0;
    std::size_t outermost_cond_start = 0;
    while (pos < src.size()) {
      std::size_t nl = 0;
      auto sv = line_at(pos, nl);
      std::string line(sv);
      if (nl < src.size()) line += '\n';  // preserve the trailing newline
      // An `import` the file already carries anchors the block too: consumers
      // are rewritten once per library they use, and the second pass must put
      // its includes above the first pass's imports, not after them.
      if (!insert_at && cond_depth == 0 && sv.starts_with("import "))
        insert_at = out.size();
      if (auto dir = preprocessor_conditional_kind(sv)) {
        if (*dir == PreprocessorConditional::kOpen) {
          if (cond_depth == 0) outermost_cond_start = out.size();
          ++cond_depth;
        } else if (*dir == PreprocessorConditional::kClose && cond_depth > 0) {
          --cond_depth;
        }
      }
      if (auto inc = parse_include_line(line)) {
        // A quoted include of another CONVERTED consumer (a test's own header)
        // imports std itself, so the block must sit above it as well: a C
        // header below an import is the one order that has to be avoided, and
        // the import can arrive through an include like this one.
        if (inc->is_quoted && cond_depth == 0 && !insert_at &&
            !options.include_to_module.count(inc->path)) {
          insert_at = out.size();
          insert_at_src_line = src_line;
        }
        if (auto *rep = find_replacement(inc->path, options.module_replacements);
            rep && !rep->guard.empty() &&
            !options.include_to_module.count(inc->path)) {
          const auto &mod = rep->module;
          replaced_in_source.insert(inc->path);
          out.push_back(std::format("#if defined({})\n", rep->guard));
          out.push_back(std::format("import {};\n", mod));
          if (rep->carries_macros_file) {
            auto mh = replacement_macros_header(inc->path, options.hyphen_macros);
            out.push_back(std::format("#if __has_include(\"{}\")\n", mh));
            out.push_back(std::format("#include \"{}\"\n", mh));
            out.push_back("#endif\n");
          }
          out.push_back("#else\n");
          out.push_back(line);
          out.push_back("#endif\n");
          pos = nl + 1;
          ++src_line;
          out.push_back(std::format("#line {}\n", src_line));
          if (pos >= src.size()) break;
          continue;
        }
        auto it = options.include_to_module.find(inc->path);
        if (it != options.include_to_module.end()) {
          auto &mod = it->second.module;
          // The name the include map gave, or the conventional one to ask
          // about when it gave none.
          const bool probed = it->second.macro_header.empty();
          std::string mh = probed ? conventional_macros_header(inc->path,
                                                               options.hyphen_macros)
                                  : it->second.macro_header;
          bool have_import = existing_imports.count(mod) ||
                             std::ranges::contains(pending_imports, mod);
          bool have_macro = existing_quoted_includes.count(mh) ||
                            std::ranges::contains(pending_macros, mh);
          if (cond_depth > 0 || it->second.reincludable) {
            // Guarded, or re-includable: replace it where it stands, so the
            // import is subject to the same condition and arrives at the same
            // point in the file.
            if (!insert_at) insert_at = outermost_cond_start;
            if (!options.dual_macro.empty())
              out.push_back(std::format("#if defined({})\n", options.dual_macro));
            if (!have_import) out.push_back(std::format("import {};\n", mod));
            if (!have_macro)
              out.push_back(probed
                                ? std::format("#if __has_include(\"{0}\")\n"
                                              "#include \"{0}\"\n#endif\n",
                                              mh)
                                : std::format("#include \"{}\"\n", mh));
            if (!options.dual_macro.empty()) {
              out.push_back("#else\n");
              out.push_back(std::string(line));
              out.push_back("#endif\n");
            }
            out.push_back(std::format("#line {}\n", src_line + 1));
            pos = nl + 1;
            ++src_line;
            if (pos >= src.size()) break;
            continue;
          }
          if (!insert_at) insert_at = out.size();
          if (!have_import) pending_imports.push_back(mod);
          if (!have_macro) {
            pending_macros.push_back(mh);
            if (probed) probed_macros.insert(mh);
          }
          replaced_includes.push_back(std::string(line));
          pos = nl + 1;
          ++src_line;
          insert_at_src_line = src_line;  // the block swallowed this line
          if (pos >= src.size()) break;
          continue;
        }
        // In a header, the C headers `import std` provides go too (see
        // ConsumerRewriteOptions::is_header).
        if (options.import_std && options.is_header && !inc->is_quoted &&
            kStdProvidedCHeaders.count(inc->path)) {
          pos = nl + 1;
          ++src_line;
          if (insert_at) out.push_back(std::format("#line {}\n", src_line));
          if (pos >= src.size()) break;
          continue;
        }
        if (options.import_std && !inc->is_quoted &&
            kStdHeaders.count(inc->path) && inc->path != "cstdio" &&
            inc->path != "cerrno" && inc->path != "cassert" &&
            inc->path != "climits") {
          pos = nl + 1;
          ++src_line;
          // Gone from the body, like the two above: the count is picked up
          // here, where the gap is, and not at the end of a block that sits
          // somewhere else entirely.
          if (insert_at) out.push_back(std::format("#line {}\n", src_line));
          if (pos >= src.size()) break;
          continue;
        }
      }
      if (auto inc = parse_include_line(line)) {
        if (!inc->is_quoted) {
          // A system include the block will end up ABOVE has to move into it.
          // Left where it is, it is read after an import — the order that
          // redefines whatever the imported module's global module fragment
          // already provided:
          //   error: redefinition of 'ssize_t read(int, void*, size_t)'
          // Only unguarded ones: a platform-guarded include keeps its guard.
          // Only a header of the standard library's. The block stands where
          // the first replaced include stood, so lifting one into it puts it
          // above the imports — right for a C header, which must not be read
          // after an import, and wrong for anyone else's: it moves the header
          // above the import it used to follow, and a library read in an order
          // its own headers never arranged for says so from the inside —
          //
          //   boost/mpl/aux_/integral_wrapper.hpp:42: error: unknown type
          //          name 'AUX_WRAPPER_VALUE_TYPE'
          //
          // that one being read once per wrapper with the wrapper's macros
          // defined around it. Where it stands is where it belongs.
          //
          // Which headers are the system's is not guessed from the shape of
          // the name -- <sys/types.h> has a slash and <boost/mpl/at.hpp> has
          // one too. It is what the trace already found the consumer taking
          // from the system, plus the standard headers named above.
          bool std_header = kStdHeaders.count(inc->path) ||
                            kStdProvidedCHeaders.count(inc->path) ||
                            options.required_system_includes.count(inc->path);
          if (insert_at && cond_depth == 0 && std_header) {
            hoisted_system_includes.push_back(line);
            pos = nl + 1;
            ++src_line;
            // The line has left the body from BELOW the block, so the count
            // has to be picked up again here rather than where the block
            // ends: what follows is no longer the line after the one before
            // it. One `#line` cannot say both.
            out.push_back(std::format("#line {}\n", src_line));
            if (pos >= src.size()) break;
            continue;
          }
          system_include_at.try_emplace(inc->path, out.size());
        }
      }
      out.push_back(line);
      pos = nl + 1;
      ++src_line;
      if (pos >= src.size()) break;
    }
  }

  // A system include the consumer already writes counts as "already there" only
  // if it survives ABOVE the block. One that stays below it — a header included
  // inside a platform conditional, which is replaced in place rather than
  // hoisted — is the ordering this block exists to avoid, and g++ does not
  // merely diagnose it:
  //
  //   bits/types/__fpos_t.h:12:11: internal compiler error:
  //       in finish_member_declaration, at cp/semantics.cc:4213
  //
  // Emitting it in the block as well costs an include the header guard drops.
  auto already_above_block = [&](const std::string &path) {
    auto it = system_include_at.find(path);
    if (it == system_include_at.end()) return false;
    return !insert_at || it->second < *insert_at;
  };

  std::string block;      // needed whichever way the file is built
  std::string modular;    // the imports, and what only they need
  for (auto &l : hoisted_system_includes) block += l;

  // Usage-based C-header re-add: `import std.compat;` cannot provide the C
  // library's macros (errno, assert, INT_MAX, ...) or POSIX declarations/types
  // (ssize_t, pthread_*) that consumers historically got transitively through
  // the library headers. The per-consumer set of required C includes is traced
  // from the consumer's source (trace_consumer_system_includes) and emitted
  // here — no symbol regexes, no macro-name heuristics.
  //
  // BEFORE the imports, not after. A C header may be a standard library wrapper
  // rather than the C library's own — libstdc++ ships `math.h` that includes
  // `<cmath>` — and once `import std.compat;` has been seen, including it again
  // textually redeclares what the module already provided:
  //
  //   ext/type_traits.h: error: type alias template redefinition with
  //                      different types
  //
  // Seen first, the textual declarations are already in place when the module
  // arrives and the two agree. The whole block is emitted at one insertion
  // point, so this is only about the order within it.
  // A header some module provides is left to the loop below, which writes the
  // import and the include as the two branches of one condition. Written here
  // as well it would arrive first and unconditionally, and the textual
  // declarations it brings are the global module's while the ones the import
  // brings are the provider's. Nothing diagnoses the two standing side by
  // side; the consumer names one and the library was built against the other,
  // and only the linker ever says so:
  //
  //   undefined reference to `boost::detail::throw_location@
  //   boost.throw_exception::throw_location(boost::source_location const&)'
  //
  // — where the library defines that constructor taking the provider's
  // `source_location@boost.assert.source_location`, and the two names differ
  // by the module each type is attached to.
  std::set<std::string> provided_by_module;
  for (auto &r : options.required_module_includes)
    provided_by_module.insert(r.headers.begin(), r.headers.end());
  // The C headers the std module also declares go first, ahead of everything
  // else the block hands over. The block carries other consumers' headers too,
  // and a consumer header of this same conversion imports std itself — so one
  // of these written after it is read after an import, and the declarations
  // they share are then made twice:
  //
  //   bits/types/struct_tm.h:7: error: redefinition of 'struct tm'
  //   string.h:105: error: redefinition of 'void* memchr(void*, int, size_t)'
  //
  // Reading them first is the whole of what they are here for.
  for (auto &inc : options.required_system_includes) {
    if (already_above_block(inc) || provided_by_module.count(inc)) continue;
    if (replaced_in_source.count(inc)) continue;
    if (!kStdProvidedCHeaders.count(inc)) continue;
    if (find_replacement(inc, options.module_replacements)) continue;
    block += std::format("#include <{}>\n", inc);
  }
  for (auto &inc : options.required_system_includes) {
    if (kStdProvidedCHeaders.count(inc)) continue;  // already above
    if (already_above_block(inc) || provided_by_module.count(inc)) continue;
    // Replaced where the source includes it: the pair is already in place and
    // a plain copy here would arrive above it, textually, and win.
    if (replaced_in_source.count(inc)) continue;
    // A module provides it, but the source never included it, so there is no
    // pair in place. It is still owed to the consumer — as the pair, for the
    // reason the plain copy is refused above.
    if (auto *rep = find_replacement(inc, options.module_replacements);
        rep && !rep->guard.empty()) {
      block += std::format("#if defined({})\n", rep->guard);
      block += std::format("import {};\n", rep->module);
      if (rep->carries_macros_file) {
        auto mh = replacement_macros_header(inc, options.hyphen_macros);
        block +=
            std::format("#if __has_include(\"{}\")\n#include \"{}\"\n#endif\n",
                        mh, mh);
      }
      block += "#else\n";
      block += std::format("#include <{}>\n", inc);
      block += "#endif\n";
      continue;
    }
    block += std::format("#include <{}>\n", inc);
  }
  for (auto &r : options.required_module_includes) {
    for (auto &inc : r.headers) {
      if (already_above_block(inc)) continue;
      block += std::format("#if defined({})\n", r.guard);
      block += std::format("import {};\n", r.module);
      if (r.carries_macros_file) {
        auto mh = replacement_macros_header(inc, options.hyphen_macros);
        block += std::format("#if __has_include(\"{}\")\n#include \"{}\"\n#endif\n",
                             mh, mh);
      }
      block += "#else\n";
      block += std::format("#include <{}>\n", inc);
      block += "#endif\n";
    }
  }
  // A translation unit that imports std but never includes <string.h> makes
  // gcc compile the module's own definition of memset, and it dies there:
  //   internal compiler error: in nonnull_arg_p, at tree.cc
  // Reading the header first is what avoids it. A header must not do this (it
  // would land below the includer's import), which is why this is .cc only.
  //
  // First in the block, not last. The block carries other consumers' headers
  // too, and a consumer header of this same conversion imports std itself, so
  // anything written after one is written after an import:
  //
  //   string.h:105: error: redefinition of
  //                 'void* memchr(void*, int, size_t)'
  //   note: previously defined here, of module std.compat, imported at
  //         googletest-param-test-test.h:35
  if (options.import_std && !options.is_header &&
      !already_above_block("string.h"))
    block.insert(0, "#include <string.h>\n");

  if (options.import_std) {
    // std.compat (rather than std) also provides the C library's global names
    // the consumer code may rely on.
    if (!existing_imports.count("std.compat"))
      modular += "import std.compat;\n";
  } else {
    for (auto &h : kStdIncludes)
      if (!already_above_block(h))
        block += std::format("#include <{}>\n", h);
  }
  for (auto &m : pending_imports) modular += std::format("import {};\n", m);
  for (auto &m : options.traced_imports)
    if (!existing_imports.count(m) &&
        !std::ranges::contains(pending_imports, m))
      modular += std::format("import {};\n", m);
  for (auto &mh : pending_macros)
    modular += probed_macros.count(mh)
                   ? std::format("#if __has_include(\"{0}\")\n"
                                 "#include \"{0}\"\n#endif\n", mh)
                   : std::format("#include \"{}\"\n", mh);

  // Dual mode: only the imports are conditional. What the file included
  // besides the library — `<boost/config/workaround.hpp>`, the C headers the
  // block hoisted — it needs whichever way it is built, and putting those
  // behind the macro leaves the classic build without them:
  //
  //   error: token is not a valid binary operator in a preprocessor
  //          subexpression
  if (options.dual_macro.empty()) {
    block += modular;
  } else if (!modular.empty() || !replaced_includes.empty()) {
    block += std::format("#if defined({})\n", options.dual_macro);
    block += modular;
    block += "#else\n";
    for (auto &l : replaced_includes) block += l;
    block += "#endif\n";
  }

  if (!block.empty()) block += std::format("#line {}\n", insert_at_src_line);

  if (!block.empty()) {
    if (insert_at)
      out.insert(out.begin() + static_cast<std::ptrdiff_t>(*insert_at), block);
    else
      out.insert(out.begin(), block);
  }

  std::string result;
  for (auto &l : out) result += l;
  return result;
}

// Inverse of `rewrite_consumer_source`, for parsing only: reconstruct the
// pre-module form of a consumer that an earlier pass already converted.
//
// A converted consumer cannot be parsed while the library is still being
// converted — `import lib;` needs a BMI that does not exist yet — so every
// later pass over that file (a second library's pass, or a re-run) would lose
// its traced imports and traced system includes to a "module not found" fatal
// error. Tracing therefore runs on this reconstruction:
//
//   * an `import` of a module in `include_to_module` becomes the include the
//     consumer originally wrote (the map is the same one the conversion used),
//   * `import std;` / `import std.compat;` becomes the standard include block,
//     which is what the consumer had before conversion dropped those includes,
//   * any other `import` is dropped: traced internal sub-modules and sibling
//     libraries have no include form of their own here, and their declarations
//     come back with the library headers being restored,
//   * generated macro-header includes are dropped, since those headers are an
//     output of the conversion and need not exist when tracing runs.
//
// A consumer that was never converted has no imports and no generated macro
// includes, so it passes through unchanged.
export std::string demodularize_consumer_source(
    const std::string &src,
    const std::map<std::string, ConsumerHeaderInfo> &include_to_module) {
  // module name → the include form that produces it, and the set of generated
  // macro headers the conversion may have added.
  std::map<std::string, std::string> include_of_module;
  std::set<std::string> macro_headers;
  for (const auto &[inc, info] : include_to_module) {
    include_of_module.emplace(info.module, inc);
    if (!info.macro_header.empty()) macro_headers.insert(info.macro_header);
  }

  std::string out;
  bool restored_std = false;
  std::size_t pos = 0;
  while (pos < src.size()) {
    auto nl = src.find('\n', pos);
    auto end = nl == std::string::npos ? src.size() : nl;
    std::string_view line(src);
    line = line.substr(pos, end - pos);
    std::string line_with_nl(line);
    if (nl != std::string::npos) line_with_nl += '\n';

    auto ns = line.find_first_not_of(" \t");
    bool dropped = false;
    if (ns != std::string_view::npos && line.substr(ns).starts_with("import ")) {
      auto rest = line.substr(ns + 7);
      auto semi = rest.find(';');
      if (semi != std::string_view::npos) {
        auto mod = rest.substr(0, semi);
        auto e = mod.find_last_not_of(" \t");
        std::string name(e == std::string_view::npos ? mod
                                                     : mod.substr(0, e + 1));
        dropped = true;
        if (auto it = include_of_module.find(name);
            it != include_of_module.end()) {
          out += std::format("#include \"{}\"\n", it->second);
        } else if (name == "std" || name == "std.compat") {
          if (!restored_std) {
            restored_std = true;
            for (auto &h : kStdIncludes) out += std::format("#include <{}>\n", h);
          }
        }
        // Any other module: dropped, see above.
      }
    } else if (auto inc = parse_include_line(line_with_nl);
               inc && inc->is_quoted &&
               (macro_headers.count(inc->path) ||
                is_generated_macro_header(inc->path))) {
      // Both this pass's own generated macro headers and those a pass for
      // another library left behind: none of them need exist while tracing
      // runs, and an include of a missing file is fatal to the parse. This
      // reconstruction only ever feeds a parse, so dropping a consumer's own
      // same-named header would at worst make that parse less complete.
      dropped = true;
    }

    if (!dropped) out += line_with_nl;
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  return out;
}

// Build the include-path → module map for a library's headers. The include form
// a consumer writes (`mylib/mylib.h`) is derived from the header's path relative
// to each `-I` include directory (most specific), then relative to the library
// root segment, then the plain filename. `headers_with_macros` is the set of
// header paths whose generated macro file should be included by consumers.
export std::map<std::string, ConsumerHeaderInfo> build_include_module_map(
    const std::vector<std::string> &header_paths,
    llvm::StringRef library_name,
    const std::vector<std::string> &extra_args,
    bool hyphen_macros,
    const std::set<std::string> &headers_with_macros) {
  std::vector<std::string> include_dirs;
  for (std::size_t i = 0; i < extra_args.size(); ++i) {
    if (extra_args[i] == "-I" && i + 1 < extra_args.size()) {
      include_dirs.push_back(extra_args[++i]);
    } else if (extra_args[i].starts_with("-I")) {
      include_dirs.push_back(extra_args[i].substr(2));
    }
  }

  struct Entry {
    std::string path;
    ConsumerHeaderInfo info;
  };
  std::vector<Entry> entries;
  entries.reserve(header_paths.size());
  for (auto &hp : header_paths) {
    auto stem = std::filesystem::path(hp).stem().string();
    ConsumerHeaderInfo info;
    info.module = derive_module_name(hp, library_name);
    if (headers_with_macros.count(hp))
      info.macro_header = include_prefix(hp, library_name) +
                          macro_base_name(stem, hyphen_macros) +
                          (hyphen_macros ? "-" : "_") + "macros.h";
    entries.push_back({hp, std::move(info)});
  }

  std::map<std::string, ConsumerHeaderInfo> out;
  auto insert = [&](const std::string &form, const ConsumerHeaderInfo &info) {
    auto it = out.find(form);
    if (it == out.end()) {
      out[form] = info;
    } else if (it->second.macro_header.empty() &&
               !info.macro_header.empty()) {
      it->second.macro_header = info.macro_header;
    }
  };

  // Most specific first: include forms under the -I directories.
  for (auto &e : entries) {
    for (auto &dir : include_dirs) {
      if (dir.empty()) continue;
      auto prefix = dir;
      if (prefix.back() != '/') prefix += "/";
      if (e.path.starts_with(prefix)) {
        auto form = std::string(e.path.substr(prefix.size()));
        if (!form.empty()) insert(form, e.info);
      }
    }
  }
  // Path relative to the library root segment.
  for (auto &e : entries) {
    auto segs = split_path(e.path);
    std::size_t start = segs.size();
    for (std::size_t i = 0; i < segs.size(); ++i) {
      if (segs[i] == library_name.str()) { start = i + 1; break; }
    }
    if (start < segs.size()) {
      std::string rel;
      for (std::size_t i = start; i < segs.size(); ++i) {
        if (!rel.empty()) rel += "/";
        rel += segs[i];
      }
      if (!rel.empty()) insert(rel, e.info);
    }
  }
  // Plain filename fallback.
  for (auto &e : entries)
    insert(std::filesystem::path(e.path).filename().string(), e.info);

  return out;
}
