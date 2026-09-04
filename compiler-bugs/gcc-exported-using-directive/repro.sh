#!/usr/bin/env bash
# An exported using-directive does not reach an importer under g++, so what the
# directive nominates is not found. clang++ carries it. Neither -std=c++20 nor
# -std=c++23 changes either answer.
set -x
CLANG=${CLANG:-clang++}
GCC=${GCC:-g++}
"$CLANG" --version | head -1
"$GCC" --version | head -1

for std in c++20 c++23; do
  # clang: the directive is carried, and `foo::Thing` is found.
  "$CLANG" -std=$std --precompile prod.cppm -o m.pcm
  "$CLANG" -std=$std -fmodule-file=m=m.pcm -fsyntax-only main.cc

  # gcc: the module builds, and then the importer cannot see the members.
  #   error: 'Thing' is not a member of 'foo'
  rm -rf gcm.cache
  "$GCC" -std=$std -fmodules-ts -c prod.cppm -o prod.o
  "$GCC" -std=$std -fmodules-ts -fsyntax-only main.cc

  # The per-name form both carry.
  rm -rf gcm.cache
  "$GCC" -std=$std -fmodules-ts -c prod_workaround.cppm -o prod.o
  "$GCC" -std=$std -fmodules-ts -fsyntax-only main.cc
done
