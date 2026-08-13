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
        : std::format("{}.{}", library_name.str(), stem);
  }

  std::vector<std::string> parts;
  for (std::size_t i = start; i < segs.size(); ++i) {
    auto s = segs[i];
    for (auto &c : s) if (c == '-') c = '_';
    if (i == segs.size() - 1) {
      auto dot = s.rfind('.');
      if (dot != std::string::npos) s = s.substr(0, dot);
    }
    parts.push_back(std::move(s));
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
