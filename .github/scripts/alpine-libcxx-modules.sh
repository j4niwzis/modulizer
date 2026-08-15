#!/bin/sh
# Give Alpine's libc++ the `import std` support it does not package.
#
# Alpine's libc++-dev ships headers and libc++.so, but none of the module
# plumbing: no std.cppm, no libc++.modules.json. CMake asks the compiler for
# that manifest, finds nothing, and `import std` is simply unavailable — which
# is why musl was excluded from every libc++ build.
#
# Nothing about the missing pieces is platform-specific. std.cppm is ordinary
# C++ that includes libc++'s own headers and re-exports them; the 110 .inc
# files listing the exported names are plain source too. They live in
# llvm-project and are reproducible from it exactly: the only substitution in
# std.cppm.in is @LIBCXX_MODULE_STD_INCLUDE_SOURCES@, the list of .inc includes
# in sorted order. Reconstructed that way against the matching release tag,
# std.cppm comes out byte-identical to the one a distribution ships.
#
# So fetch them at the tag matching the installed clang, generate the two .cppm
# files, and write the manifest where the compiler says it expects it.
set -eu

V=$(clang++ --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
[ -n "$V" ] || { echo "could not determine the clang version" >&2; exit 1; }

# Ask clang where it looks — but when nothing is there it answers with the
# literal string "<NOT PRESENT>" rather than a path or an empty line, so a
# plain emptiness check passes and the manifest gets written to a file called
# "<NOT PRESENT>". Fall back to the directory holding libc++ itself, which is
# where clang reports the manifest once it exists.
MANIFEST=$(clang++ -stdlib=libc++ -print-library-module-manifest-path 2>/dev/null || true)
case "$MANIFEST" in
  ""|*"NOT PRESENT"*)
    LIB=$(clang++ -stdlib=libc++ -print-file-name=libc++.so)
    case "$LIB" in
      */*) MANIFEST=$(dirname "$LIB")/libc++.modules.json ;;
      *) echo "cannot locate libc++.so to place the manifest beside" >&2; exit 1 ;;
    esac
    ;;
esac

DEST=/usr/share/libc++/v1
SRC=/tmp/llvm-modules

# Blobless and sparse: libcxx/modules is well under a megabyte, and the rest of
# llvm-project is not worth downloading for it.
rm -rf "$SRC"
git clone --quiet --depth 1 --filter=blob:none --sparse \
  https://github.com/llvm/llvm-project -b "llvmorg-$V" "$SRC"
git -C "$SRC" sparse-checkout set libcxx/modules
M=$SRC/libcxx/modules
[ -f "$M/std.cppm.in" ] || { echo "no std.cppm.in at llvmorg-$V" >&2; exit 1; }

mkdir -p "$DEST"
cp -r "$M/std" "$M/std.compat" "$DEST/"

# Substitute the include list. Sorted, which is the order the generated file
# uses, so the result matches what a distribution would have installed.
gen() { # $1 = std | std.compat, $2 = the variable to replace
  block=$(cd "$DEST" && ls "$1"/*.inc | LC_ALL=C sort | sed 's|^|#include "|; s|$|"|')
  awk -v var="$2" -v block="$block" \
    '{ if (index($0, var)) print block; else print }' \
    "$M/$1.cppm.in" > "$DEST/$1.cppm"
}
gen std '@LIBCXX_MODULE_STD_INCLUDE_SOURCES@'
gen std.compat '@LIBCXX_MODULE_STD_COMPAT_INCLUDE_SOURCES@'

# The manifest resolves source-path relative to its own directory; an absolute
# path is equally valid and spares us a relative-path computation busybox's
# realpath cannot do.
mkdir -p "$(dirname "$MANIFEST")"
sed "s|@LIBCXX_MODULE_RELATIVE_PATH@|$DEST|g" "$M/modules.json.in" > "$MANIFEST"

echo "libc++ $V module sources installed:"
echo "  manifest: $MANIFEST"
echo "  sources:  $DEST/std.cppm ($(ls "$DEST"/std/*.inc | wc -l) includes)"
