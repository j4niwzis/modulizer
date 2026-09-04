#!/usr/bin/env bash
# Convert googletest with the modulizer in this checkout, and tar the result.
#
#   generate-tree.sh <glibc|musl> [libstdc++|libc++]
#
# The point is to run this somewhere OTHER than where the tree gets built. What
# a converter emits must not depend on the machine it ran on, and that is not a
# theoretical worry: the include set a unit keeps is decided by resolving each
# use back to the header that owns it, which is exactly the kind of answer a
# host can bake in. A tree generated under musl and built under glibc (and the
# reverse) is what catches it.
#
# Output: generated-tree-<libc>.tar.gz in the working directory.
set -euo pipefail

LIBC=${1:?usage: $0 <glibc|musl> [libstdc++|libc++]}
# Which standard library the CONVERSION PARSES AGAINST. Independent of the one
# modulizer itself was built with: modulizer links clang's libraries, which are
# built against libstdc++, but the code it reads is parsed with whatever the
# flags after `--` say. Generation only reads headers, so it needs no `std`
# module and libc++ works here even where `import std` could not be built.
STDLIB=${2:-libstdc++}
case "$STDLIB" in
  libstdc++) PARSE_STDLIB="" ;;
  libc++)    PARSE_STDLIB="-stdlib=libc++" ;;
  *) echo "unknown stdlib: $STDLIB" >&2; exit 2 ;;
esac
FORK=${FORK:-https://github.com/j4niwzis/googletest-modules.git}
GTEST_REPO=${GTEST_REPO:-https://github.com/google/googletest.git}
GTEST_BASE=${GTEST_BASE:-49495eac}
# Convert for GCC: the extern "C++" wrapping becomes selectable at build time
# so the same tree serves a conforming implementation and GCC both.
GCC_MODULES=${GCC_MODULES:-0}
[ "$GCC_MODULES" = 1 ] && GCC_FLAG=--gcc-modules || GCC_FLAG=
OUT=generated-tree-$LIBC-$STDLIB${GCC_FLAG:+-gcc}.tar.gz

# Any non-empty std.compat list switches the emitted `import std;` to
# `import std.compat;`; which C++ headers are actually replaced is detected per
# file, and C headers are kept.
STD_COMPAT="std.compat=string,string_view,vector,memory,ostream,sstream,set,map,type_traits,functional,limits,cstddef,cstdint,ios,deque,utility,tuple,algorithm,iterator,iomanip,stdexcept,chrono,thread,mutex,list,unordered_set,unordered_map,variant,optional,any,initializer_list,new,typeinfo,array,complex,regex,ratio,numeric,cmath,cstring,cstdlib,cwchar,cctype,random,bitset,atomic,condition_variable,future,exception,system_error,filesystem,forward_list,charconv,concepts,ranges,span,bit,compare"

read -r -d '' GENERATE <<EOF || true
set -eux

# An unversioned clang++ has to be on PATH. modulizer lives outside the LLVM
# tree, so libtooling derives the rest of its include resolution by probing the
# driver; with no clang++ to probe, a use resolves to a different file than the
# include as written and the unit silently loses the system headers its code
# needs. The parse still succeeds, so nothing looks wrong until the tree is
# built somewhere and fails on 'getcwd' must be declared before it is used.
command -v clang++ >/dev/null || { echo "no clang++ on PATH" >&2; exit 1; }

cmake -S /src/standalone -B /build-modulizer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS=-fno-rtti \
  \$LLVM_CMAKE_ARGS
# CMake 4.4 synthesizes a second \`std\` target for the executable, so main.cc is
# handed a different \`std\` BMI than the library's module units were built
# against. Clang segfaults in ASTReader rather than diagnosing it:
#
#   main.cc:6:8: current parser token 'vector'
#   clang frontend command failed with exit code 133
#
# Point main.cc back at the base BMI and rebuild. The retry is guarded so a
# genuine build failure still fails, rather than being masked by the patch.
if ! cmake --build /build-modulizer -j"\$(nproc)"; then
  MM=/build-modulizer/CMakeFiles/modulizer_client.dir/src/main.cc.o.modmap
  grep -q '@cmake_cxx_std@synth' "\$MM" || {
    echo "build failed for some other reason than the synthesized std target" >&2
    exit 1; }
  sed -i 's|std=CMakeFiles/@cmake_cxx_std@synth_0.dir/[^" ]*|std=CMakeFiles/@cmake_cxx_std.dir/std.pcm|' "\$MM"
  rm -f /build-modulizer/CMakeFiles/modulizer_client.dir/src/main.cc.o
  cmake --build /build-modulizer -j"\$(nproc)"
fi
M=/build-modulizer/modulizer_client
[ -x "\$M" ] || { echo "modulizer_client was not built" >&2; exit 1; }

git clone --quiet "$GTEST_REPO" /gt
git -C /gt checkout --quiet "$GTEST_BASE"
cd /gt

RES=\$(clang++ -print-resource-dir)
ARGS="-x c++ -std=c++20 $PARSE_STDLIB -Wno-pragma-once-outside-header -resource-dir=\$RES
      -DGTEST_ENABLE_CATCH_EXCEPTIONS_=1
      -Igoogletest/include -Igoogletest -Igooglemock/include -Igooglemock"
FLAGS="--dual-impl --import-std --wrapper-module --hyphen-macros
       --internal-mode=both $GCC_FLAG --module-replaces=$STD_COMPAT"
STAGE=\$(mktemp -d)

gtest_headers="googletest/include/gtest/*.h googletest/include/gtest/internal/*.h googletest/src/gtest-internal-inl.h"
gtest_sources="googletest/src/gtest-assertion-result.cc googletest/src/gtest-death-test.cc googletest/src/gtest-filepath.cc googletest/src/gtest-matchers.cc googletest/src/gtest-port.cc googletest/src/gtest-printers.cc googletest/src/gtest-test-part.cc googletest/src/gtest-typed-test.cc googletest/src/gtest.cc googletest/src/gtest_main.cc"
gmock_headers="googlemock/include/gmock/*.h googlemock/include/gmock/internal/*.h"
gmock_sources="googlemock/src/gmock-cardinalities.cc googlemock/src/gmock-internal-utils.cc googlemock/src/gmock-matchers.cc googlemock/src/gmock-spec-builders.cc googlemock/src/gmock.cc googlemock/src/gmock_main.cc"
gtest_consumers="googletest/test/*.cc googletest/test/*.h googletest/samples/*.cc googletest/samples/*.h"
gmock_consumers="googlemock/test/*.cc googlemock/test/*.h"

ics=""; for f in \$gtest_consumers \$gmock_consumers googlemock/src/*.cc; do ics="\$ics --internal-consumer-source=\$f"; done
"\$M" --full --module-name=gtest --output-dir="\$STAGE/gtest" \$FLAGS \$ics \
  \$gtest_headers \$gtest_sources -- \$ARGS

ics=""; for f in \$gmock_consumers; do ics="\$ics --internal-consumer-source=\$f"; done
"\$M" --full --module-name=gmock --output-dir="\$STAGE/gmock" \$FLAGS \$ics \
  \$gmock_headers \$gmock_sources -- \$ARGS

lhs=""; for f in \$gmock_headers; do lhs="\$lhs --library-header=\$f"; done
"\$M" --consumers --module-name=gmock --import-std --hyphen-macros \
  "--module-replaces=$STD_COMPAT" \$lhs \$gmock_consumers -- \$ARGS

lhs=""; for f in \$gtest_headers; do lhs="\$lhs --library-header=\$f"; done
"\$M" --consumers --module-name=gtest --import-std --hyphen-macros \
  "--module-replaces=$STD_COMPAT" \$lhs \$gtest_consumers \$gmock_consumers -- \$ARGS

# Install the units where the fork keeps them. The membership tests below are
# space-separated on purpose: built with newlines they never match, and every
# rewritten header lands at the include root instead of under its own prefix.
install_lib() { # \$1 = googletest|googlemock, \$2 = gtest|gmock
  dir=\$1; lib=\$2; st="\$STAGE/\$2"
  pub=\$(cd "\$dir/include/\$lib" && ls *.h 2>/dev/null | tr '\n' ' ')
  int=\$(cd "\$dir/include/\$lib/internal" && ls *.h 2>/dev/null | tr '\n' ' ')
  src=\$(cd "\$dir/src" && ls *.h 2>/dev/null | tr '\n' ' ')

  rm -f "\$dir"/src/*.cc "\$dir/src/\$lib-all.cc"
  rm -f "\$dir/include/\$lib"/*.h "\$dir/include/\$lib/internal"/*.h
  rm -f "\$dir/src/\$lib-internal-inl.h"
  mkdir -p "\$dir/src/internal" "\$dir/src/impl" "\$dir/include/\$lib/internal"

  cp "\$st"/*.cc "\$dir/src/"
  [ -d "\$st/internal" ] && cp "\$st"/internal/*.cc "\$dir/src/internal/"
  cp "\$st"/impl/*.cc "\$dir/src/impl/"
  [ -d "\$st/impl/modules" ] && mkdir -p "\$dir/src/impl/modules" && cp "\$st"/impl/modules/*.cc "\$dir/src/impl/modules/"
  cp "\$st/\$lib"/*.h "\$dir/include/\$lib/"
  [ -d "\$st/\$lib/internal" ] && cp "\$st/\$lib"/internal/*.h "\$dir/include/\$lib/internal/"

  for f in "\$st"/*.h; do
    [ -e "\$f" ] || continue
    b=\$(basename "\$f")
    case " \$pub " in *" \$b "*) cp "\$f" "\$dir/include/\$lib/"; continue ;; esac
    case " \$src " in *" \$b "*) cp "\$f" "\$dir/src/"; continue ;; esac
    cp "\$f" "\$dir/include/"
  done
  for f in "\$st"/internal/*.h; do
    [ -e "\$f" ] || continue
    b=\$(basename "\$f")
    case " \$int " in
      *" \$b "*) cp "\$f" "\$dir/include/\$lib/internal/" ;;
      *) cp "\$f" "\$dir/include/" ;;
    esac
  done
  return 0
}
install_lib googletest gtest
install_lib googlemock gmock

# The CMake that builds the emitted units is hand-written, not generated, so it
# comes from the fork.
git clone --quiet --depth 1 "$FORK" /fork
for f in CMakeLists.txt googletest/CMakeLists.txt googlemock/CMakeLists.txt \
         googletest/cmake/Config.cmake.in googlemock/cmake/Config.cmake.in; do
  mkdir -p "/gt/\$(dirname \$f)"
  cp "/fork/\$f" "/gt/\$f"
done

rm -rf /gt/.git
tar -C /gt -czf "/src/$OUT" .
EOF

case "$LIBC" in
  glibc)
    docker run --rm -v "$PWD:/src" ubuntu:26.04 bash -euxc "
      export LLVM_CMAKE_ARGS='-DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm -DClang_DIR=/usr/lib/llvm-22/lib/cmake/clang'
      apt-get update
      DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl git gnupg lsb-release ninja-build wget \
        software-properties-common libstdc++-15-dev pkg-config
      curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh
      chmod +x /tmp/llvm.sh
      /tmp/llvm.sh 22 all
      DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libc++-22-dev libc++abi-22-dev
      # apt.llvm.org installs versioned binaries only; the unversioned name is
      # what libtooling probes for (see the check in the generate step).
      ln -sf /usr/lib/llvm-22/bin/clang++ /usr/local/bin/clang++
      ln -sf /usr/lib/llvm-22/bin/clang /usr/local/bin/clang
      curl -fsSL -o /tmp/cmake.tar.gz \
        https://github.com/Kitware/CMake/releases/download/v4.4.2/cmake-4.4.2-linux-x86_64.tar.gz
      mkdir -p /opt/cmake && tar -xf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1
      export PATH=/opt/cmake/bin:\$PATH
      $GENERATE
    "
    ;;
  musl)
    # Same package set as the musl unit job: modulizer is built here, not just
    # the tree, so the -dev/-static splits LLVMExports.cmake names are needed.
    docker run --rm -v "$PWD:/src" alpine:edge sh -euxc "
      export LLVM_CMAKE_ARGS='-DLLVM_DIR=/usr/lib/llvm22/lib/cmake/llvm -DClang_DIR=/usr/lib/llvm22/lib/cmake/clang'
      apk add --no-cache clang22 clang22-dev clang22-extra-tools clang22-static \
        llvm22-dev llvm22-static llvm22-gtest binutils g++ cmake \
        ninja-build ninja-is-really-ninja git musl-dev linux-headers \
        libc++-dev pkgconf
      $GENERATE
    "
    ;;
  *) echo "unknown libc: $LIBC" >&2; exit 2 ;;
esac

echo "generated $OUT"
