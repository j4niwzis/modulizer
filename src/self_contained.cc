module;

export module modulizer.self_contained;
import modulizer.include_analysis;
import modulizer.util;
import libtooling;
import std;

// Not every header is a translation unit. A library often has headers written
// to be included from one particular point and nowhere else — they name an
// entity whose definition the header that includes them brought in first, and
// on their own they do not compile:
//
//   error: calling 'default_error_condition' with incomplete return type
//          'error_condition'
//
// A module unit IS a translation unit, always, so such a header has to be
// given what its usage site gave it. This pass finds that context: for each
// header that fails to parse alone, it looks at the headers that include it
// and takes what each of those included first. The result is fed back as
// `-include` flags, so every later analysis sees a sound AST and the ordinary
// injected-import machinery works out which of those become imports.
//
// A context is only ever a PREFIX of some header's includes, so it can never
// contain the header itself — feeding a header its own text would leave the
// parse with an empty body once the include guard had done its work.
//
// Some headers have no context that would do. Those are reported separately
// and left as textual includes; see HeaderContexts::fragments.

namespace {

// Counts errors without printing them: a header failing to parse alone is what
// this pass is looking for, not something to report.
class ErrorCounter : public clang::DiagnosticConsumer {
public:
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic &info) override {
    if (level >= clang::DiagnosticsEngine::Error) ++errors;
    clang::DiagnosticConsumer::HandleDiagnostic(level, info);
  }
  unsigned errors = 0;
};

// Whether the header compiles with `preamble` included ahead of it.
bool parses_with(const std::string &path,
                 const std::vector<std::string> &extra_args,
                 const std::vector<std::string> &preamble) {
  auto flags = base_compile_flags();
  flags.insert(flags.end(), extra_args.begin(), extra_args.end());
  for (const auto &p : preamble) {
    flags.push_back("-include");
    flags.push_back(p);
  }
  clang::tooling::FixedCompilationDatabase db(".", flags);
  clang::tooling::ClangTool tool(db, {path});
  ErrorCounter counter;
  tool.setDiagnosticConsumer(&counter);
  auto factory =
      clang::tooling::newFrontendActionFactory<clang::SyntaxOnlyAction>();
  tool.run(factory.get());
  return counter.errors == 0;
}

// A header that opens pragma state another header closes is one half of a
// bracket: `detail/header.hpp` pushes the warning and packing state that
// `detail/footer.hpp` pops, and each is written to be pasted around something
// else. Neither is a translation unit, whatever its own parse says — the one
// that pushes ends with the state still open, the one that pops has nothing to
// pop — and neither can be a module, since pragma state does not cross a
// module boundary at all.
bool is_bracket_half(const std::string &src) {
  int depth = 0;
  // A bracket kept by a macro rather than by pragma state: one half sets a
  // flag the other clears, and each refuses to be read out of turn --
  //
  //   #ifdef BOOST_ITERATOR_CONFIG_DEF
  //   # error you have nested config_def #inclusion.
  //   #else
  //   # define BOOST_ITERATOR_CONFIG_DEF
  //   #endif
  //
  // A macros file that replays that flag makes the next legitimate reading of
  // the header a nested one, and the header says so:
  //
  //   config_def_macros.h:16: error: you have nested config_def #inclusion.
  //
  // The whole shape is what marks it: a bare test of ONE macro, an #error in
  // one arm, and the same macro set or cleared in the other. A header that
  // merely raises an #error about something it does not control is ordinary --
  // gtest-port.h refuses C++ before 17 and refuses a libc++ without wide
  // characters, and it is a header like any other.
  std::string flag;              // the macro the open conditional tests
  bool saw_error = false;        // and whether it refuses inside that test
  bool saw_toggle = false;       // and sets or clears the same one
  bool macro_bracket = false;
  auto first_word = [](std::string_view a) {
    auto b = a.find_first_not_of(" \t");
    if (b == std::string_view::npos) return std::string_view();
    auto e = a.find_first_of(" \t\r", b);
    return a.substr(b, e == std::string_view::npos ? e : e - b);
  };
  std::size_t pos = 0;
  while (pos < src.size()) {
    auto nl = src.find('\n', pos);
    if (nl == std::string::npos) nl = src.size();
    auto line = std::string_view(src).substr(pos, nl - pos);
    pos = nl + 1;
    auto d = parse_directive(line, /*skip_hash_ws=*/true,
                             /*keyword_ends_crlf=*/true);
    if (!d) continue;
    if (d->keyword == "ifdef" || d->keyword == "ifndef") {
      flag = std::string(first_word(d->after));
      saw_error = saw_toggle = false;
    } else if (d->keyword == "if") {
      flag.clear();  // a compound condition is not this shape
    } else if (d->keyword == "error" && !flag.empty()) {
      saw_error = true;
    } else if ((d->keyword == "define" || d->keyword == "undef") &&
               !flag.empty() && first_word(d->after) == flag) {
      saw_toggle = true;
    } else if (d->keyword == "endif") {
      if (saw_error && saw_toggle) macro_bracket = true;
      flag.clear();
      saw_error = saw_toggle = false;
    }
    if (d->keyword != "pragma") continue;
    // Only the state-stacking pragmas: `#pragma once` and
    // `#pragma warning(disable: …)` stack nothing.
    if (d->after.contains("push")) ++depth;
    if (d->after.contains("pop")) --depth;
  }
  return depth != 0 || macro_bracket;
}

// One place a header is included from: which header does it, and what that
// header had already included by then.
struct UsageSite {
  std::string site;
  std::vector<std::string> before;
};

// Every usage site of every header, read from the text. A header that does not
// parse cannot be asked for its includes any other way.
std::map<std::string, std::vector<UsageSite>> collect_usage_sites(
    const std::vector<std::string> &paths,
    const std::vector<std::string> &extra_args) {
  std::set<std::string> known(paths.begin(), paths.end());
  std::map<std::string, std::vector<UsageSite>> sites;
  for (const auto &p : paths) {
    auto src = read_file(p);
    if (src.empty()) continue;
    std::vector<std::string> before;
    for (const auto &inc : parse_includes(src)) {
      auto resolved = resolve_include(inc.path, p, extra_args);
      if (resolved.empty() || !known.count(resolved)) continue;
      sites[resolved].push_back({p, before});
      before.push_back(resolved);
    }
  }
  return sites;
}

// What to try for one usage site: the site's own context, then what it had
// included by the time it reached this header. A site is only as good as the
// header it sits in — where that header is itself included from somewhere
// particular, what came before it there counts too.
std::vector<std::string> context_of_site(
    const UsageSite &usage,
    const std::map<std::string, std::size_t> &index,
    const std::vector<std::vector<std::string>> &site_contexts) {
  std::vector<std::string> pre;
  if (auto it = index.find(usage.site); it != index.end())
    pre = site_contexts[it->second];
  for (const auto &b : usage.before)
    if (!std::ranges::contains(pre, b)) pre.push_back(b);
  return pre;
}

// What one header turned out to be, given the contexts found for the others so
// far. `context` empty and `fragment` false means it needs nothing.
struct Verdict {
  std::vector<std::string> context;
  bool fragment = false;
};

Verdict classify_one(const std::string &path,
                     const std::vector<std::string> &extra_args,
                     const std::vector<UsageSite> *usages,
                     const std::map<std::string, std::size_t> &index,
                     const std::vector<std::vector<std::string>> &site_contexts) {
  if (!usages) return {{}, true};  // parses nowhere and nothing includes it
  std::vector<std::vector<std::string>> tries;
  for (const auto &usage : *usages)
    tries.push_back(context_of_site(usage, index, site_contexts));
  // The longest first: a header included late has the most brought in before
  // it, and that is what it was written for, so the context kept is the one
  // that says the most.
  std::ranges::sort(tries, [](const auto &a, const auto &b) {
    return a.size() > b.size();
  });
  // Every site is tried, not just the first that works. A header that parses
  // where one header includes it and not where another does is not a
  // translation unit at all — it is text, and whichever context it were given
  // would be the wrong one everywhere else.
  Verdict v;
  bool any = false, all = true;
  for (const auto &pre : tries) {
    if (!pre.empty() && parses_with(path, extra_args, pre)) {
      if (!any) v.context = pre;
      any = true;
    } else {
      all = false;
    }
  }
  if (!any || !all) return {{}, true};
  return v;
}

}  // namespace

// Whether a consumer could write `#include <name>` and have it compile with
// nothing else around it. An include guard does not settle this: it says "read
// me once", not "read me alone", and a fragment can carry one and still name
// terms only its includer defines --
//
//   detail/utf8_codecvt_facet.hpp:99: error: unknown type name
//     'BOOST_UTF8_BEGIN_NAMESPACE'
//
// -- so the question is put to the compiler, in the form the consumer would
// write it.
export bool include_stands_alone(const std::string &name,
                                 const std::vector<std::string> &extra_args) {
  auto flags = base_compile_flags();
  flags.insert(flags.end(), extra_args.begin(), extra_args.end());
  clang::tooling::FixedCompilationDatabase db(".", flags);
  // The name the tool will open, not the one written here: it makes a relative
  // argument absolute against the working directory, and a virtual file mapped
  // under the other spelling is simply not there -- which reads as "this header
  // does not compile" for every header asked about.
  const std::string probe = "__mz_stands_alone.cc";
  // Twice. Once says whether it parses; a consumer needs more than that, since
  // its build reads a header again wherever the next thing needs it. Half a
  // bracket compiles perfectly well alone and says what it thinks of the
  // second reading:
  //
  //   config/abi_prefix.hpp:12: error: double inclusion of header
  //     boost/config/abi_prefix.hpp is an error
  //
  // and its partner, which takes the guard back, does not survive the first.
  // Neither is a header a consumer can be handed.
  auto text = std::format("#include <{0}>\n#include <{0}>\n", name);
  clang::tooling::ClangTool tool(db, {probe});
  // Both spellings: the tool makes a relative argument absolute against the
  // working directory, and a virtual file mapped under only one of them is
  // simply not there -- which reads as "this header does not compile", for
  // every header asked about.
  tool.mapVirtualFile(probe, text);
  std::error_code ec;
  auto abs = std::filesystem::absolute(probe, ec);
  if (!ec && abs.string() != probe) tool.mapVirtualFile(abs.string(), text);
  ErrorCounter counter;
  tool.setDiagnosticConsumer(&counter);
  auto factory =
      clang::tooling::newFrontendActionFactory<clang::SyntaxOnlyAction>();
  tool.run(factory.get());
  return counter.errors == 0;
}

// What the sweep found. `contexts` maps a header that needs one to the library
// headers that have to be included ahead of it. `fragments` are headers no
// context would fix: text meant to be pasted where it lands, the way an
// X-macro `.inc` file or an ABI bracket half is. They stay textual includes.
export struct HeaderContexts {
  std::map<std::string, std::vector<std::string>> contexts;
  std::set<std::string> fragments;
};

export HeaderContexts discover_header_contexts(
    const std::vector<std::string> &paths,
    const std::vector<std::string> &extra_args) {
  auto sites = collect_usage_sites(paths, extra_args);

  std::vector<char> settled_alone(paths.size(), 0);
  std::vector<char> bracket(paths.size(), 0);
  parallel_for(paths.size(), [&](std::size_t i) {
    bracket[i] = is_bracket_half(read_file(paths[i])) ? 1 : 0;
    if (bracket[i]) return;  // a bracket half is never a module: do not parse
    settled_alone[i] = parses_with(paths[i], extra_args, {}) ? 1 : 0;
  });

  std::map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < paths.size(); ++i) index[paths[i]] = i;

  // A header's context depends on the contexts of the headers that include it,
  // which are being worked out at the same time, so this settles over a few
  // rounds rather than in one.
  std::vector<std::vector<std::string>> contexts(paths.size());
  std::vector<char> fragment(paths.size(), 0);
  for (int round = 0; round < 3; ++round) {
    std::vector<std::vector<std::string>> next_contexts(paths.size());
    std::vector<char> next_fragment(paths.size(), 0);
    parallel_for(paths.size(), [&](std::size_t i) {
      if (bracket[i]) { next_fragment[i] = 1; return; }
      if (settled_alone[i]) return;
      auto it = sites.find(paths[i]);
      auto v = classify_one(paths[i], extra_args,
                            it == sites.end() ? nullptr : &it->second, index,
                            contexts);
      next_contexts[i] = std::move(v.context);
      next_fragment[i] = v.fragment ? 1 : 0;
    });
    bool same = next_contexts == contexts && next_fragment == fragment;
    contexts = std::move(next_contexts);
    fragment = std::move(next_fragment);
    if (same) break;
  }

  HeaderContexts out;
  for (std::size_t i = 0; i < paths.size(); ++i) {
    if (fragment[i]) out.fragments.insert(paths[i]);
    else if (!contexts[i].empty())
      out.contexts[paths[i]] = std::move(contexts[i]);
  }
  return out;
}

// The contexts as compiler flags, ready to append to a parse of that header.
export std::map<std::string, std::vector<std::string>> context_args(
    const std::map<std::string, std::vector<std::string>> &contexts) {
  std::map<std::string, std::vector<std::string>> out;
  for (const auto &[path, pre] : contexts) {
    auto &args = out[path];
    for (const auto &p : pre) {
      args.push_back("-include");
      args.push_back(p);
    }
  }
  return out;
}
