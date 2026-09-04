module;

export module modulizer.naming;
import modulizer.util;
import libtooling;
import std;

// Mode controlling how "internal module" is determined:
//   dir  — directory segment contains an internal/impl marker.
//   stem — filename stem contains an internal/impl marker.
//   both — either of the above (default).
export enum class InternalMode { kDir, kStem, kBoth };

export InternalMode parse_internal_mode(llvm::StringRef s) {
  if (s == "dir") return InternalMode::kDir;
  if (s == "stem") return InternalMode::kStem;
  return InternalMode::kBoth;
}

// True for the name of a macro header this tool generates: `<stem>-macros.h`
// (--hyphen-macros) or `<stem>_macros.h`. Used to recognise the tool's own
// output in a source it is asked to read back, where the file may not exist
// yet — the macro headers are produced by the header rewrite, which can run
// after (or independently of) a consumers pass.
export bool is_generated_macro_header(llvm::StringRef include_path) {
  return include_path.ends_with("-macros.h") ||
         include_path.ends_with("_macros.h");
}

export std::string macro_prefix(llvm::StringRef module_name) {
  auto mapped = module_name | std::views::transform([](char c) -> char {
    if (c == '.' || c == '-' || c == ':') return '_';
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  });
  return std::ranges::to<std::string>(mapped);
}

// A module is "internal" depending on the configured mode:
//   dir  — internal if a directory segment (path or module name) contains
//          an internal/impl marker. Filenames never count, so a public
//          header like mylib_pred_impl.h is NOT internal.
//   stem — internal if the filename stem contains an internal/impl marker
//          (e.g. a_internal.h, mylib-internal.h).
//   both — either of the above.
export bool is_internal_module(const std::string &path,
                               const std::string &module_name,
                               InternalMode mode = InternalMode::kBoth) {
  auto check_dir = [](const std::vector<std::string> &segs, bool is_path) {
    // Path: directories are all but the last (filename) segment.
    // Module name: directories are the middle segments (skip library + stem).
    std::size_t begin = is_path ? 0 : 1;
    std::size_t end = segs.size() > 0 ? segs.size() - 1 : 0;
    for (std::size_t i = begin; i < end; ++i) {
      if (is_internal_segment(segs[i])) return true;
    }
    return false;
  };
  auto check_stem = [](const std::vector<std::string> &segs) {
    return !segs.empty() && is_internal_segment(segs.back());
  };

  auto path_segs = split_on(path, '/');
  auto name_segs = split_on(module_name, '.');

  if (mode == InternalMode::kDir || mode == InternalMode::kBoth) {
    if (check_dir(path_segs, true)) return true;
    if (check_dir(name_segs, false)) return true;
  }
  if (mode == InternalMode::kStem || mode == InternalMode::kBoth) {
    if (check_stem(path_segs)) return true;
    if (check_stem(name_segs)) return true;
  }
  return false;
}

// Derive the module name for a header from its path and the library name.
//   library/folder/module.h  =>  library.folder.module
// The first path segment equal to the library name marks the library root;
// everything after it (subdirectories + filename stem) becomes module
// segments. A header whose stem equals the library name (e.g. mylib/mylib.h)
// is the top-level module and is named just the library name.
// A module name is a dot-separated list of identifiers, and a keyword is not
// one: `boost/core/alignof.hpp` would name the module `boost.core.alignof`,
// which the compiler reads as a keyword where an identifier belongs. The unit
// then declares no module at all —
//
//   CMake Error: Output …/alignof.cc.o is of type `CXX_MODULES` but does not
//   provide a module interface unit or partition
//
// — so a component that collides gets a trailing underscore, the usual way of
// keeping a name that reads like the file it came from.
std::string escape_module_part(std::string s) {
  static const std::set<std::string> kKeywords = {
      "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
      "bool", "break", "case", "catch", "char", "char16_t", "char32_t",
      "char8_t", "class", "compl", "concept", "const", "const_cast",
      "consteval", "constexpr", "constinit", "continue", "co_await",
      "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
      "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false",
      "float", "for", "friend", "goto", "if", "inline", "int", "long",
      "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
      "operator", "or", "or_eq", "private", "protected", "public", "register",
      "reinterpret_cast", "requires", "return", "short", "signed", "sizeof",
      "static", "static_assert", "static_cast", "struct", "switch", "template",
      "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
      "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
      "wchar_t", "while", "xor", "xor_eq",
  };
  if (kKeywords.count(s)) s += "_";
  return s;
}

export std::string derive_module_name(llvm::StringRef header_path,
                                      llvm::StringRef library_name) {
  auto segs = split_path(header_path);

  std::size_t start = segs.size();
  for (std::size_t i = 0; i < segs.size(); ++i) {
    if (segs[i] == library_name.str()) { start = i + 1; break; }
  }

  if (start == segs.size()) {
    // Library segment not found: fall back to stem-based naming.
    auto stem = std::filesystem::path(header_path.str()).stem().string();
    for (auto &c : stem) if (c == '-') c = '_';
    return stem == library_name.str()
        ? library_name.str()
        : std::format("{}.{}", library_name.str(), escape_module_part(stem));
  }

  std::vector<std::string> parts;
  for (std::size_t i = start; i < segs.size(); ++i) {
    auto s = segs[i];
    for (auto &c : s) if (c == '-') c = '_';
    if (i == segs.size() - 1) {
      auto dot = s.rfind('.');
      if (dot != std::string::npos) s = s.substr(0, dot);
    }
    parts.push_back(escape_module_part(std::move(s)));
  }
  if (parts.empty()) return library_name.str();
  if (parts.size() == 1 && parts[0] == library_name.str())
    return library_name.str();
  std::string mod = library_name.str();
  for (auto &p : parts) {
    mod += ".";
    mod += p;
  }
  return mod;
}

// The macro a translation unit defines to say that IT imports this module.
// Distinct from the provider's `_IMPORT_MODULES`, which says only that the
// provider is a module somewhere in this build -- true of every unit in it,
// including the ones that import nothing and read the header textually.
//
// Named for the module and not the library, because a library is many modules
// and importing one of them says nothing about the rest. Guarding a library's
// headers under a single name would take an include away from a unit that
// imported some other part of the same library and still needs this one.
export std::string imported_flag(std::string_view module_name) {
  std::string flag;
  flag.reserve(module_name.size() + 9);
  for (char c : module_name)
    flag += c == '.' ? '_' : static_cast<char>(std::toupper(c));
  return flag + "_IMPORTED";
}
