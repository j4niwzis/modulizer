module;

export module modulizer.consumer_rewriter;
import modulizer.include_analysis;
import modulizer.naming;
import modulizer.util;
import libtooling;
import std;

// Standard-library headers that `import std;` provides. When --import-std is
// set, a consumer's `#include <...>` lines for these headers are dropped and
// replaced by a single `import std;`.
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

namespace {

// The block of standard headers a consumer needs once the library headers no
// longer pull them in textually (module interface units keep them in the GMF).
// Emitted when --import-std is NOT set; --import-std replaces it with
// `import std;`. Matches the includes consumer code previously received
// transitively from the library headers.
const std::vector<std::string> kStdIncludes = {
    "algorithm", "cstdio", "cstdlib", "cstring", "exception", "functional",
    "iostream", "iterator", "limits", "map", "memory", "ostream", "set",
    "sstream", "string", "tuple", "type_traits", "utility", "vector",
};

} // namespace

export struct ConsumerHeaderInfo {
  std::string module;
  std::string macro_header;  // empty when the header has no generated macro file
};

export struct ConsumerRewriteOptions {
  bool import_std = false;
  std::map<std::string, ConsumerHeaderInfo> include_to_module;
  // System C/POSIX headers the consumer uses macros/symbols from (traced from
  // its source via trace_consumer_system_includes). `import std.compat` cannot
  // provide the C library's macros (errno, assert, INT_MAX, NULL, stderr) or
  // POSIX declarations/types (ssize_t, pthread_*), so these includes must be
  // re-added once the library headers no longer pull them in textually.
  std::set<std::string> required_system_includes;
  // Modules the consumer references internal entities of (traced from the
  // consumer's source against the library headers). Those entities are
  // exported from their defining sub-modules but not re-exported by the
  // wrapper (internal-consumer reachability), so the consumer must import the
  // owning modules itself.
  std::vector<std::string> traced_imports;
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
  std::vector<std::string> out;
  std::optional<std::size_t> insert_at;
  {
    std::size_t pos = 0;
    // Depth of open #if/#ifdef/#ifndef blocks, and where the outermost
    // currently-open one started in `out` (the last unconditional position).
    int cond_depth = 0;
    std::size_t outermost_cond_start = 0;
    while (pos < src.size()) {
      std::size_t nl = 0;
      auto sv = line_at(pos, nl);
      std::string line(sv);
      if (nl < src.size()) line += '\n';  // preserve the trailing newline
      if (auto dir = preprocessor_conditional_kind(sv)) {
        if (*dir == PreprocessorConditional::kOpen) {
          if (cond_depth == 0) outermost_cond_start = out.size();
          ++cond_depth;
        } else if (*dir == PreprocessorConditional::kClose && cond_depth > 0) {
          --cond_depth;
        }
      }
      if (auto inc = parse_include_line(line)) {
        auto it = options.include_to_module.find(inc->path);
        if (it != options.include_to_module.end()) {
          auto &mod = it->second.module;
          auto &mh = it->second.macro_header;
          bool have_import = existing_imports.count(mod) ||
                             std::ranges::contains(pending_imports, mod);
          bool have_macro = mh.empty() || existing_quoted_includes.count(mh) ||
                            std::ranges::contains(pending_macros, mh);
          if (cond_depth > 0) {
            // Guarded include: replace it where it stands so the import is
            // subject to the same condition.
            if (!insert_at) insert_at = outermost_cond_start;
            if (!have_import) out.push_back(std::format("import {};\n", mod));
            if (!have_macro) out.push_back(std::format("#include \"{}\"\n", mh));
            pos = nl + 1;
            if (pos >= src.size()) break;
            continue;
          }
          if (!insert_at) insert_at = out.size();
          if (!have_import) pending_imports.push_back(mod);
          if (!have_macro) pending_macros.push_back(mh);
          pos = nl + 1;
          if (pos >= src.size()) break;
          continue;
        }
        if (options.import_std && !inc->is_quoted &&
            kStdHeaders.count(inc->path) && inc->path != "cstdio" &&
            inc->path != "cerrno" && inc->path != "cassert" &&
            inc->path != "climits") {
          pos = nl + 1;
          if (pos >= src.size()) break;
          continue;
        }
      }
      out.push_back(line);
      pos = nl + 1;
      if (pos >= src.size()) break;
    }
  }

  std::string block;
  if (options.import_std) {
    // std.compat (rather than std) also provides the C library's global names
    // the consumer code may rely on.
    if (!existing_imports.count("std.compat"))
      block += "import std.compat;\n";
  } else {
    for (auto &h : kStdIncludes)
      if (!existing_std_includes.count(h))
        block += std::format("#include <{}>\n", h);
  }
  for (auto &m : pending_imports) block += std::format("import {};\n", m);
  for (auto &m : options.traced_imports)
    if (!existing_imports.count(m) &&
        !std::ranges::contains(pending_imports, m))
      block += std::format("import {};\n", m);
  for (auto &mh : pending_macros)
    block += std::format("#include \"{}\"\n", mh);

  // Usage-based C-header re-add: `import std.compat;` cannot provide the C
  // library's macros (errno, assert, INT_MAX, ...) or POSIX declarations/types
  // (ssize_t, pthread_*) that consumers historically got transitively through
  // the library headers. The per-consumer set of required C includes is traced
  // from the consumer's source (trace_consumer_system_includes) and emitted
  // here — no symbol regexes, no macro-name heuristics.
  for (auto &inc : options.required_system_includes)
    if (!existing_std_includes.count(inc))
      block += std::format("#include <{}>\n", inc);

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
               inc && inc->is_quoted && macro_headers.count(inc->path)) {
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
