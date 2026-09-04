# [gcc][modules] An exported using-directive is not carried to an importer

A `using namespace` directive exported from a module interface has no effect in
a translation unit that imports the module: the names it nominates are not
found. `g++` accepts the module interface without complaint and then rejects
the use.

`[module.interface]/2` makes the directive an exported declaration — a
using-directive is a *block-declaration*, so one at namespace scope inside an
`export-declaration` is exported — and the example in that subclause marks
`export using namespace N;` as OK. `clang++` carries it. The behaviour does not
change between `-std=c++20` and `-std=c++23`.

(C++20 read the other way, marking the same line "error: does not declare a
name"; the requirement that an exported declaration introduce a name went in
C++23. Neither compiler diagnoses the C++20 form, so the split below is about
propagation, not about which standard is selected.)

## Reproducer

`prod.cppm`
```cpp
export module m;

namespace foo {
namespace bar {
export struct Thing { int v; };
}
export using namespace bar;
}  // namespace foo
```

`main.cc`
```cpp
import m;
int main() { foo::Thing t{1}; return t.v - 1; }
```

```sh
g++ -std=c++23 -fmodules-ts -c prod.cppm -o prod.o     # accepted
g++ -std=c++23 -fmodules-ts -fsyntax-only main.cc      # error
```

## Actual

```
main.cc: error: 'Thing' is not a member of 'foo'
```

## Expected

The importer finds `foo::Thing` through the exported directive, as it does with
`clang++`.

## Observed

| form | clang 22 | gcc 15 |
| --- | --- | --- |
| `export using namespace bar;` | found, c++20 and c++23 | not found, both |
| `export namespace foo { ... using namespace bar; }` | found, both | not found, both |
| `export using bar::Thing;` (per name) | found | found |

Wrapping the directive in an exported namespace does not help; only a
using-*declaration* per name crosses on both.

## Versions

- `g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0`
- `Ubuntu clang version 22.1.2 (1ubuntu1)`

## Where this bites

A library that hides names behind an ADL barrier and re-exposes them with a
using-directive loses them for every importer built with gcc. The workaround is
to emit one exported using-declaration per name alongside the directive; it
belongs behind a gcc-targeting option rather than being applied unconditionally,
since the directive alone is what the standard asks for.
