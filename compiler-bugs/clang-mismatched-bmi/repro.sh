#!/usr/bin/env bash
# Segfault in clang when a TU is given a BMI for a transitively imported module
# that differs from the BMI its dependency was built against.
set -x
CXX=${CXX:-clang++}
"$CXX" --version | head -2

# Two builds of module `dep` that differ in content (only `dep1.pcm` is what
# `lib` is built against).
"$CXX" -std=c++23 -x c++-module dep.cppm --precompile -o dep1.pcm
"$CXX" -std=c++23 -DEXTRA -x c++-module dep.cppm --precompile -o dep2.pcm

# `lib` is built against dep1.pcm.
"$CXX" -std=c++23 -x c++-module lib.cppm --precompile -fmodule-file=dep=dep1.pcm -o lib.pcm

# The consumer is handed dep2.pcm instead => clang segfaults (exit 139/254).
"$CXX" -std=c++23 -fmodule-file=lib=lib.pcm -fmodule-file=dep=dep2.pcm -c main.cc -o main.o
