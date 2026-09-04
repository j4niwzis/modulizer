#!/usr/bin/env bash
# g++ refuses an implementation unit that imports a module whose closure comes
# back to the implementation unit's own module. clang++ accepts it. The
# standard forbids only a DIRECT self-import.
set -x
CLANG=${CLANG:-clang++}
GCC=${GCC:-g++}
"$CLANG" --version | head -1
"$GCC" --version | head -1

# clang: accepted.
"$CLANG" -std=c++23 --precompile a.cppm -o A.pcm
"$CLANG" -std=c++23 -fmodule-file=A=A.pcm --precompile b.cppm -o B.pcm
"$CLANG" -std=c++23 -fmodule-file=A=A.pcm -fmodule-file=B=B.pcm -c a_impl.cc -o a_impl.o

# gcc: the two interfaces build, the implementation unit does not.
#   B: error: cannot import module in its own purview
rm -rf gcm.cache
"$GCC" -std=c++23 -fmodules-ts -c a.cppm -o a.o
"$GCC" -std=c++23 -fmodules-ts -c b.cppm -o b.o
"$GCC" -std=c++23 -fmodules-ts -c a_impl.cc -o a_impl.o
