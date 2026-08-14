# modulizer

A libtooling-based C++ tool that rewrites C/C++ libraries to use **C++20 modules**,
while keeping them consumable by existing non-module consumers.

`modulizer` parses a library's headers, extracts its public API (classes,
functions, enums, aliases, variables, macros), analyzes which internal/detail
entities are reachable — through public macros and real consumer code — and
emits per-header module interface units (`.cc`) and implementation units, plus
generated macro and export helper headers.

The `googletest-modules` fork is generated end-to-end by `modulizer` itself and
builds/passes gtest's own test suite.

---

## Features

- **Entity extraction** — `RecursiveASTVisitor`-based extraction of namespace-scope
  classes, structs, functions, enums, aliases, variables, `using`-declarations and
  `using`-directives, with multi-pass `#ifdef` handling for platform guards.
- **Macro handling** — public macros are moved to generated macro headers with
  export markers baked into their bodies; private (`#undef`'d) macros and include
  guards are filtered out. `#ifdef`-gated entity groups are re-emitted as
  conditional blocks.
- **Reachability analysis** —
  - macro reachability (FQN + wildcard extraction, call-graph BFS, overload and
    transitive-type expansion),
  - consumer tracing (which internal entities a library's own tests/main and
    external consumers actually use),
  - cross-module analysis of `extern "C++"` shared entities (forward-declared,
    out-of-line member/class definitions, free functions, friend declarations,
    template-body references).
- **Module emission** —
  - headers → module **interface units** (`.cc`) with a global module fragment
    (GMF) holding the *used* system/third-party includes,
  - `.cpp`/`.cc`/`.cxx` → module **implementation units**,
  - forward-declaration injection for entities used before they are visible,
  - optional `extern "C++"` wrapping of shared entities,
  - optional `--cc-only`: headers rewritten entirely into interface units,
    no `.h` emitted.
- **Wrapper generation** — a facade wrapper module (and a legacy
  `using ::ns::Entity;` wrapper) that re-exports public entities, so consumers
  keep working via one import.
- **`import std`** — `--import-std` replaces C++ stdlib includes with
  `import std;`; `--module-replaces="std.compat=..."` maps stdlib headers to
  modules.

---

## Usage

The CLI modes are selected by flags; everything after `--` is the clang compile
command used for the fixed compilation database.

### List extracted entities (dry run)

```sh
./build/standalone/modulizer_client --list -- \
  -I include -x c++ -std=c++20 include/my_lib/a.h
```

### Headers rewrite (`--headers`)

Convert library headers into per-file `.h`/`.cc` module pairs:

```sh
./build/standalone/modulizer_client --headers \
  --module-name=my_lib --output-dir=out \
  [--internal-mode=dir|stem|both] [--import-std] \
  [--module-replaces="std.compat=string,vector,..."] \
  include/my_lib/a.h include/my_lib/b.h -- \
  -I include -x c++ -std=c++20 -Wno-pragma-once-outside-header
```

### Full rewrite (`--full`)

Convert headers into module interface units **and** implementation files into
module implementation units:

```sh
./build/standalone/modulizer_client --full \
  --module-name=my_lib --output-dir=out \
  [--extern-cxx] [--import-std] [--cc-only] \
  include/my_lib/a.h include/my_lib/b.h src/impl.cc -- \
  -I include -x c++ -std=c++20 -Wno-pragma-once-outside-header
```

- `--extern-cxx` wraps shared entities in `extern "C++"` (OFF by default in
  `--full`; ON by default in `--headers`, disable with `--no-extern-cxx`).
- `--cc-only` inlines headers entirely into interface units (no `.h` emitted).
- Implementation units are written under `out/impl/`; implementation-only
  modules get an empty `export module X;` interface so clang can compile the
  module.
- `--dual-impl` keeps the implementation files usable *both* ways. `module;`
  must be the first token of a translation unit — clang rejects it even behind
  a taken `#ifdef` — so one file cannot serve both modes, and the rewrite is
  split in two:
  - `out/impl/<stem>.cc` is a plain translation unit: it keeps its includes,
    wrapped in `#ifndef LIB_USE_MODULES`, and is otherwise the original body.
  - `out/impl/modules/<stem>.cc` is the module implementation unit: global
    module fragment, `module X;`, the imports, and then a textual include of
    the body (with `LIB_USE_MODULES` defined, so the body's own includes
    compile out).

  A classic build compiles the first file and uses the original headers; a
  modules build compiles the second. The body exists once either way.

### Wrapper generation (`--wrapper`)

Generate a wrapper module that re-exports a set of headers:

```sh
./build/standalone/modulizer_client --wrapper \
  --module-name=libtooling --output-dir=out \
  <headers...> -- -I <clang-include> -x c++ -std=c++20
```

Produces `<module>.cc` (a module re-exporting every entity) and
`<module>_macros.h` (the library's public macros, verbatim `#define`s with
guards and `#undef`'d implementation macros filtered out).

---

## How it works

1. **Analyze** each header: extract entities + macros, tracking namespace paths
   and internal depth (dirs named `internal`/`detail`/`impl`).
2. **Reachability**: run macro reachability and consumer tracing to compute the
   set of internal entities that must be exported so consumers (and macros)
   work via module imports.
3. **Cross-module**: compute `extern "C++"` shared entities across headers —
   forward declarations, out-of-line definitions, friends, template-body refs.
4. **Rewrite**: for each header, emit
   - `module;` + GMF (used system includes, macro header),
   - `export module <name>;` + purview imports,
   - the rewritten header body (with export markers and stripped macros),
   - a generated macro header and export header (`<LIB>-export.h`).
5. **Route macro-body markers** across files (a macro defined in one header and
   invoked in another carries its export marker wherever it expands).
6. **Write** outputs and (optionally) the wrapper facade.

---

## Related projects

Both are consumed via CPM:

- **[libtooling-wrapper](https://github.com/j4niwzis/libtooling-wrapper)** —
  the thin clang/llvm wrapper module that `modulizer` builds on, generated by
  `modulizer`'s own `--wrapper` mode.
- **[googletest-modules](https://github.com/j4niwzis/googletest-modules)** —
  googletest/googlemock rewritten as C++ modules by `modulizer`, used as the
  test-suite dependency. The fork builds and passes gtest's own test suite,
  serving as the project's end-to-end validation.
