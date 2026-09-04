# [gcc][modules] An implementation unit cannot import a module whose closure re-enters its own module

An implementation unit of module `A` that imports `B`, where `B` imports `A`, is
rejected: `B: error: cannot import module in its own purview`. Nothing here is
a direct self-import — the implementation unit imports `B`, and it is `B`'s own
import of `A` that gcc objects to. `clang++` accepts it.

`[module.unit]` forbids a module unit from importing its own module directly.
An implementation unit is *implicitly* an importer of its interface, so the
graph here is `A(impl) -> B -> A`, which the standard does not forbid.

## Reproducer

`a.cppm`
```cpp
export module A;
export int a();
```

`b.cppm`
```cpp
export module B;
import A;
export inline int b() { return a(); }
```

`a_impl.cc`
```cpp
module A;
import B;
int a() { return 1; }
```

```sh
g++ -std=c++23 -fmodules-ts -c a.cppm -o a.o        # ok
g++ -std=c++23 -fmodules-ts -c b.cppm -o b.o        # ok
g++ -std=c++23 -fmodules-ts -c a_impl.cc -o a_impl.o
```

## Actual

```
In module imported at a_impl.cc:5:1:
B: error: cannot import module in its own purview
```

## Expected

Accepted, as with `clang++`.

## Versions

- `g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0`
- `Ubuntu clang version 22.1.2 (1ubuntu1)`

## Where this bites

A library whose lowest-level module is used by the headers it is implemented in
hits this structurally: the implementation file of the base module reaches for
helpers that all import the base module back. Dropping those imports only moves
the failure to missing declarations.

Two ways out, neither needing the defect fixed: emit such implementation files
as plain translation units rather than module implementation units (a non-module
TU has no module of its own, so there is no cycle), or coalesce the cyclically
dependent headers into one module.
