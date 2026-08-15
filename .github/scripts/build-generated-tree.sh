#!/usr/bin/env bash
# Build modulizer's generated googletest tree and run its own test suites.
#
#   build-generated-tree.sh <glibc|musl> <libstdc++|libc++>
#
# By default this builds the published fork. Set TREE_TARBALL to a tarball
# produced by generate-tree.sh to build THAT instead — which is how the
# cross-environment jobs work: convert in one container, build in another, so
# nothing the conversion host happened to answer can pass unnoticed.
#
# The tree is generated on one machine against one standard library and has to
# compile everywhere else: that is what emitting no standard-library internals
# (`bits/…`, `__algorithm/…`) and none of their private guards buys. Building it
# against a different libc and a different standard library than it was
# generated with is the test.
#
# glibc runs in an ubuntu:26.04 container (libstdc++ 15, whose `bits/std.cc`
# CMake compiles as the `std` module); musl runs in alpine:edge. Both go
# through `docker run` rather than a job container, so the workflow's
# JavaScript actions keep running on the glibc runner.
set -euo pipefail

LIBC=${1:?usage: $0 <glibc|musl> <libstdc++|libc++>}
STDLIB=${2:?usage: $0 <glibc|musl> <libstdc++|libc++>}
FORK=${FORK:-https://github.com/j4niwzis/googletest-modules.git}
TREE_TARBALL=${TREE_TARBALL:-}
# Set to 0 to build only the classic half. Alpine ships libc++ headers but no
# `std.cppm` or `libc++.modules.json`, so CMake cannot build the `std` module
# there and `import std.compat` has nothing to resolve against — the classic
# half needs none of that and still exercises the emitted headers.
MODULE_BUILD=${MODULE_BUILD:-1}

case "$STDLIB" in
  libstdc++) CXX_FLAGS="" ;;
  libc++)    CXX_FLAGS="-stdlib=libc++" ;;
  *) echo "unknown stdlib: $STDLIB" >&2; exit 2 ;;
esac

# The tarball's libc++ is not on the loader's path, so the test executables
# need an rpath to it or ctest fails with `libc++.so.1: cannot open shared
# object file`.
LINK_FLAGS="$CXX_FLAGS"
if [ "$STDLIB" = "libc++" ] && [ "$LIBC" = "glibc" ]; then
  LINK_FLAGS="$LINK_FLAGS -Wl,-rpath,/usr/lib/llvm-22/lib"
fi

# The build itself, run inside whichever container the libc selects. Both a C
# and a C++ compiler are named: the tree's top-level project() enables C as
# well, and these images have no `cc` to fall back on. The generated tree picks
# the `import std` UUID from the CMake it finds, so the distributions shipping
# different CMake versions is fine.
if [ -n "$TREE_TARBALL" ]; then
  FETCH_TREE='mkdir -p /gt && tar -C /gt -xzf /tree.tar.gz'
  MOUNT_TREE="-v $(cd "$(dirname "$TREE_TARBALL")" && pwd)/$(basename "$TREE_TARBALL"):/tree.tar.gz:ro"
else
  FETCH_TREE="git clone --depth 1 \"$FORK\" /gt"
  MOUNT_TREE=""
fi

# Alpine needs libc++ put together by hand: the unwinder it links against is a
# separate package (without it the first compiler check dies on "cannot find
# -lunwind", and the name has moved between releases), and no std.cppm or
# libc++.modules.json is packaged at all.
#
# Built as a variable rather than written inline in the `docker run` command:
# that command is a double-quoted string, so quotes inside it would close and
# reopen it, and the shell that eventually runs would see something other than
# what is written here.
if [ "$LIBC" = musl ] && [ "$STDLIB" = libc++ ]; then
  read -r -d '' LIBCXX_SETUP <<'SETUP' || true
apk add --no-cache libc++-dev
apk add --no-cache llvm-libunwind-dev || apk add --no-cache libunwind-dev
sh /repo/.github/scripts/alpine-libcxx-modules.sh
SETUP
else
  LIBCXX_SETUP=':'
fi

if [ "$MODULE_BUILD" = "1" ]; then
  read -r -d '' MODULE_PART <<EOF || true
cmake -S /gt -B /gt/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_GMOCK=ON \
  -Dgtest_build_modules=ON \
  -Dgtest_build_tests=ON \
  -Dgmock_build_tests=ON \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="$CXX_FLAGS" \
  -DCMAKE_EXE_LINKER_FLAGS="$LINK_FLAGS"
cmake --build /gt/build -j"\$(nproc)"
ctest --test-dir /gt/build --output-on-failure -j"\$(nproc)"
EOF
else
  MODULE_PART='echo "skipping the module build: no std module for this standard library"'
fi

read -r -d '' BUILD <<EOF || true
set -eux
$FETCH_TREE
$MODULE_PART

# The other half of the dual tree: the same sources as a classic
# header-and-source library, which must not need any module machinery.
cmake -S /gt -B /gt/build-classic -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_GMOCK=ON \
  -Dgtest_build_modules=OFF \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="$CXX_FLAGS" \
  -DCMAKE_EXE_LINKER_FLAGS="$LINK_FLAGS"
cmake --build /gt/build-classic -j"\$(nproc)"
EOF

case "$LIBC" in
  glibc)
    docker run --rm $MOUNT_TREE ubuntu:26.04 bash -euxc "
      apt-get update
      DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl git gnupg lsb-release ninja-build wget \
        software-properties-common libstdc++-15-dev
      curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh
      chmod +x /tmp/llvm.sh
      /tmp/llvm.sh 22 all
      DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libc++-22-dev libc++abi-22-dev
      # The libc++.modules.json clang reports lives in /usr/lib/x86_64-linux-gnu
      # and names its sources relatively — \"../share/libc++/v1/std.cppm\" —
      # which resolves to /lib/share/libc++/v1, a directory no package creates.
      # The .cppm files ship under the versioned prefix instead, so point the
      # path the manifest actually uses at them; otherwise CMake fails with
      # \"Cannot find source file: /lib/share/libc++/v1/std.compat.cppm\".
      mkdir -p /lib/share/libc++
      ln -sfn /usr/lib/llvm-22/share/libc++/v1 /lib/share/libc++/v1
      ln -sf /usr/lib/llvm-22/bin/clang++ /usr/local/bin/clang++
      ln -sf /usr/lib/llvm-22/bin/clang /usr/local/bin/clang
      curl -fsSL -o /tmp/cmake.tar.gz \
        https://github.com/Kitware/CMake/releases/download/v4.4.2/cmake-4.4.2-linux-x86_64.tar.gz
      mkdir -p /opt/cmake && tar -xf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1
      export PATH=/opt/cmake/bin:\$PATH
      $BUILD
    "
    ;;
  musl)
    # binutils supplies the `ld` clang drives — without it the very first
    # compiler check dies with `posix_spawn failed: No such file or directory`.
    # g++ supplies the GCC installation clang links against on Alpine (crt
    # files, libgcc) and libstdc++'s headers, including the `bits/std.cc` that
    # becomes the `std` module. Ninja must be the real one: Alpine's `samurai`
    # owns the `ninja` name but reports itself as 1.9.0 and lacks the dyndep
    # support C++20 modules need, which CMake rejects outright ("Ninja 1.11 or
    # higher is required"). ninja-is-really-ninja repoints `ninja` at it.
    # clang-scan-deps does the C++20 module dependency scanning CMake drives,
    # and Alpine ships it in clang22-extra-tools rather than clang22 — without
    # it the build dies immediately with
    # CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS-NOTFOUND.
    docker run --rm $MOUNT_TREE -v "$PWD:/repo:ro" alpine:edge sh -euxc "
      apk add --no-cache clang22 clang22-extra-tools binutils g++ cmake \
        ninja-build ninja-is-really-ninja git musl-dev linux-headers
      $LIBCXX_SETUP
      $BUILD
    "
    ;;
  *) echo "unknown libc: $LIBC" >&2; exit 2 ;;
esac
