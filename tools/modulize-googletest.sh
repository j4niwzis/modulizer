#!/usr/bin/env bash
#
# Reproduce the `googletest-modules` fork (https://github.com/j4niwzis/googletest-modules,
# commit "Modulize") from a pristine googletest checkout, using modulizer.
#
# The recipe below was reconstructed from the fork's generated artifacts (module
# names, file layout, macro-header naming, export markers) and cross-checked
# against modulizer's own code paths — see the notes at each step.
#
# Usage:
#   MODULIZER=/path/to/build/standalone/modulizer_client \
#     tools/modulize-googletest.sh /path/to/googletest
#
# The googletest checkout is rewritten IN PLACE (tests/samples are converted to
# imports; headers are replaced by module interface units), so run it on a fresh
# clone:
#
#   git clone https://github.com/google/googletest.git
#   git -C googletest checkout d89aac5   # parent of the fork's "Modulize" commit
#
# Not covered here: the fork's hand-written CMakeLists.txt files (root,
# googletest/, googlemock/) and cmake/Config.cmake.in. Those were written by
# hand to build the emitted module units — modulizer does not generate them.

set -euo pipefail

MODULIZER=${MODULIZER:-modulizer_client}
GT=${1:?usage: MODULIZER=... $0 <googletest-checkout>}
STD=${STD:-c++20}

command -v "$MODULIZER" >/dev/null 2>&1 || [ -x "$MODULIZER" ] || {
  echo "error: modulizer_client not found (set MODULIZER=/path/to/modulizer_client)" >&2
  exit 1
}
MODULIZER=$(command -v "$MODULIZER" || readlink -f "$MODULIZER")

cd "$GT"
[ -f googletest/include/gtest/gtest.h ] || {
  echo "error: $GT does not look like a googletest checkout" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
echo "staging generated units in $STAGE"

# ---------------------------------------------------------------------------
# Compile commands (everything after `--`). modulizer builds a fixed compilation
# database from these; -I googletest is needed because the tests include
# "src/gtest-internal-inl.h".
# ---------------------------------------------------------------------------
# A clang resource dir is only needed when modulizer_client lives outside its
# LLVM tree (libclang locates its builtin headers relative to the executable);
# without it every parse fails with `'stddef.h' file not found`.
RESOURCE_ARGS=()
if [ -n "${CLANG_RESOURCE_DIR:-}" ]; then
  RESOURCE_ARGS=("-resource-dir=$CLANG_RESOURCE_DIR")
fi

# Both libraries' include roots are on the path for every run: the gtest run
# traces googlemock's tests as consumers (they use gtest internals), so it has
# to be able to resolve their `gmock/...` includes too. `-Igoogletest` /
# `-Igooglemock` (the library roots) are needed for the tests' "src/..." and
# "test/..." includes. GTEST_ENABLE_CATCH_EXCEPTIONS_ is supplied by
# googletest's own CMake for the death-test-exception tests.
GTEST_ARGS=(-x c++ "-std=$STD" -Wno-pragma-once-outside-header "${RESOURCE_ARGS[@]}"
            -DGTEST_ENABLE_CATCH_EXCEPTIONS_=1
            -Igoogletest/include -Igoogletest
            -Igooglemock/include -Igooglemock)
GMOCK_ARGS=("${GTEST_ARGS[@]}")

# Any non-empty std.compat list switches the emitted `import std;` to
# `import std.compat;` (rewrite_includes.cc: std_module_name); the actual set of
# replaced C++ stdlib headers is auto-detected, C headers/<version> are kept.
STD_COMPAT="std.compat=string,string_view,vector,memory,ostream,sstream,set,map,type_traits,functional,limits,cstddef,cstdint,ios,deque,utility,tuple,algorithm,iterator,iomanip,stdexcept,chrono,thread,mutex,list,unordered_set,unordered_map,variant,optional,any,initializer_list,new,typeinfo,array,complex,regex,ratio,numeric,cmath,cstring,cstdlib,cwchar,cctype,random,bitset,atomic,condition_variable,future,exception,system_error,filesystem,forward_list,charconv,concepts,ranges,span,bit,compare"

# Shared flags of both `--full` runs. Evidence for each:
#   --cc-only       headers are gone from the fork; every header became a .cc
#                   module interface unit (no .h emitted).
#   --extern-cxx    the units contain injected `extern "C++"` shared-entity
#                   declarations (off by default in --full).
#   --import-std    `import std.compat;` + `#ifndef GTEST_IMPORT_STD` guarded
#                   stdlib includes in the global module fragment.
#   --wrapper-module  gtest.h became `gtest-umbrella.cc` / module gtest.umbrella,
#                   plus the generated facade `gtest.cc` / `export module gtest;`.
#   --hyphen-macros macro headers are `gtest-message-macros.h`, and
#                   gtest_pred_impl.h → `gtest-pred-impl-macros.h` (underscores
#                   in the stem turned into hyphens).
# NOT used: --export-all (internal entities like testing::internal::GetArgvs are
# left unexported and get `extern "C++"` instead), --combined-macros,
# --macro-prefix (GTEST_/GMOCK_ are the default uppercased library names),
# --internal-mode (the default `both` and `dir` agree on this tree).
FULL_FLAGS=(--cc-only --extern-cxx --import-std --wrapper-module --hyphen-macros
            "--module-replaces=$STD_COMPAT")

# ---------------------------------------------------------------------------
# Input sets, exactly the files the fork emitted units for.
#   - internal/custom/*.h are user-customisation points and were NOT converted.
#   - gtest-all.cc / gmock-all.cc (the "everything" TUs) were dropped.
# ---------------------------------------------------------------------------
gtest_headers=(googletest/include/gtest/*.h googletest/include/gtest/internal/*.h
               googletest/src/gtest-internal-inl.h)
gtest_sources=(googletest/src/gtest-assertion-result.cc googletest/src/gtest-death-test.cc
               googletest/src/gtest-filepath.cc googletest/src/gtest-matchers.cc
               googletest/src/gtest-port.cc googletest/src/gtest-printers.cc
               googletest/src/gtest-test-part.cc googletest/src/gtest-typed-test.cc
               googletest/src/gtest.cc googletest/src/gtest_main.cc)

gmock_headers=(googlemock/include/gmock/*.h googlemock/include/gmock/internal/*.h)
gmock_sources=(googlemock/src/gmock-cardinalities.cc googlemock/src/gmock-internal-utils.cc
               googlemock/src/gmock-matchers.cc googlemock/src/gmock-spec-builders.cc
               googlemock/src/gmock.cc googlemock/src/gmock_main.cc)

# Consumers: the libraries' own tests, test helper headers and samples. Passing
# them as INTERNAL consumers is what makes test-only internals (e.g.
# testing::internal::TestResultAccessor) exported from their defining module
# while staying out of the `gtest` facade — exactly what the fork shows.
gtest_consumers=(googletest/test/*.cc googletest/test/*.h
                 googletest/samples/*.cc googletest/samples/*.h)
gmock_consumers=(googlemock/test/*.cc googlemock/test/*.h)

internal_consumer_flags() {
  for f in "$@"; do printf -- '--internal-consumer-source=%s\n' "$f"; done
}
library_header_flags() {
  for f in "$@"; do printf -- '--library-header=%s\n' "$f"; done
}

# ---------------------------------------------------------------------------
# 1. googletest → modules.
# ---------------------------------------------------------------------------
echo "== googletest: --full =="
mapfile -t gtest_ics < <(internal_consumer_flags "${gtest_consumers[@]}" "${gmock_consumers[@]}")
"$MODULIZER" --full \
  --module-name=gtest --output-dir="$STAGE/gtest" \
  "${FULL_FLAGS[@]}" "${gtest_ics[@]}" \
  "${gtest_headers[@]}" "${gtest_sources[@]}" \
  -- "${GTEST_ARGS[@]}"

# ---------------------------------------------------------------------------
# 2. googlemock → modules. gmock's includes of "gtest/..." are auto-derived into
#    `import gtest.…;` (compute_auto_imports treats the first path segment of a
#    quoted include as a sibling library root), so no explicit mapping is needed.
#    This runs against the still-pristine gtest headers.
# ---------------------------------------------------------------------------
echo "== googlemock: --full =="
mapfile -t gmock_ics < <(internal_consumer_flags "${gmock_consumers[@]}")
"$MODULIZER" --full \
  --module-name=gmock --output-dir="$STAGE/gmock" \
  "${FULL_FLAGS[@]}" "${gmock_ics[@]}" \
  "${gmock_headers[@]}" "${gmock_sources[@]}" \
  -- "${GMOCK_ARGS[@]}"

# ---------------------------------------------------------------------------
# 3. Consumer conversion (in place, one pass per library — the pass is
#    idempotent, so the gtest pass leaves the already-converted gmock imports
#    alone). --library-header is required because test helper *headers*
#    (gmock-matchers_test.h, production.h, sample1.h, …) are consumers too.
# ---------------------------------------------------------------------------
echo "== googlemock: --consumers =="
mapfile -t gmock_lhs < <(library_header_flags "${gmock_headers[@]}")
"$MODULIZER" --consumers \
  --module-name=gmock --import-std --hyphen-macros \
  "--module-replaces=$STD_COMPAT" \
  "${gmock_lhs[@]}" "${gmock_consumers[@]}" \
  -- "${GMOCK_ARGS[@]}"

echo "== googletest: --consumers =="
mapfile -t gtest_lhs < <(library_header_flags "${gtest_headers[@]}")
"$MODULIZER" --consumers \
  --module-name=gtest --import-std --hyphen-macros \
  "--module-replaces=$STD_COMPAT" \
  "${gtest_lhs[@]}" "${gtest_consumers[@]}" "${gmock_consumers[@]}" \
  -- "${GTEST_ARGS[@]}"

# ---------------------------------------------------------------------------
# 4. Install the generated units into the layout the fork uses.
#
#    modulizer writes, relative to --output-dir:
#      <stem>.cc, internal/<stem>.cc   module interface units (mirroring the
#                                      header tree under the library root)
#      <lib>-umbrella.cc               interface unit of the umbrella header
#      <lib>.cc                        the facade wrapper module
#      impl/<stem>.cc                  module implementation units
#      <lib>/…-macros.h, <lib>-export.h  generated macro/export headers, placed
#                                      at their header's include prefix
#    The fork keeps the .cc units under <lib>/src/ and the generated headers
#    under <lib>/include/, which is what the moves below do.
# ---------------------------------------------------------------------------
install_lib() { # $1 = googletest|googlemock, $2 = gtest|gmock
  local dir=$1 lib=$2 st="$STAGE/$2"

  # Originals that became modules are removed first: the generated wrapper
  # <lib>.cc and the empty interface <lib>_main.cc take the names of the
  # original implementation files.
  rm -f "$dir"/src/*.cc "$dir"/src/"$lib"-all.cc
  rm -f "$dir"/include/"$lib"/*.h "$dir"/include/"$lib"/internal/*.h
  rm -f "$dir"/src/"$lib"-internal-inl.h

  mkdir -p "$dir/src/internal" "$dir/src/impl" "$dir/include/$lib/internal"

  cp "$st"/*.cc "$dir/src/"
  [ -d "$st/internal" ] && cp "$st"/internal/*.cc "$dir/src/internal/"
  cp "$st"/impl/*.cc "$dir/src/impl/"

  # Generated headers: <lib>/… keeps its include prefix, top-level ones (from
  # headers outside the include tree, e.g. src/gtest-internal-inl.h) go to the
  # include root.
  cp "$st/$lib"/*.h "$dir/include/$lib/"
  [ -d "$st/$lib/internal" ] && cp "$st/$lib"/internal/*.h "$dir/include/$lib/internal/"
  shopt -s nullglob
  local top=("$st"/*.h)
  shopt -u nullglob
  [ ${#top[@]} -gt 0 ] && cp "${top[@]}" "$dir/include/"
  return 0
}

echo "== installing generated units =="
install_lib googletest gtest
install_lib googlemock gmock

cat <<'EOF'

Done. The tree now matches the fork's generated content:
  googletest/src/{,internal/,impl/}*.cc   module interface + implementation units
  googletest/include/gtest/*-macros.h     generated macro headers
  googletest/include/{gtest-export.h,gtest/gtest-export.h,gtest-internal-inl-macros.h}
  googlemock/…                            likewise
  googletest/test, googlemock/test, googletest/samples  converted to imports

Still to do by hand (not produced by modulizer): the fork's CMakeLists.txt files
and cmake/Config.cmake.in, which declare the FILE_SET CXX_MODULES targets.
EOF
