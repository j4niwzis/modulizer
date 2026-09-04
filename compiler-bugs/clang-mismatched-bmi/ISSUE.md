# [clang][modules] Crash in `ASTReader` when a TU is given a different BMI for a transitively imported module

Clang segfaults instead of diagnosing when a translation unit is handed a BMI
for module `dep` that differs from the BMI that module `lib` (which it imports)
was built against. The crash happens on the first reference to an entity whose
declaration must be deserialized.

This is reachable from ordinary builds: CMake 4.4's `import std` support
synthesizes a second `std` target (`@cmake_cxx_std@synth_0`) for some consumers.
When a library's module units were compiled against the base `std` BMI and a
consuming target is passed the synthesized one, CMake reports

```
CMake Error: Disagreement of the location of the 'std' module.
Location A: 'CMakeFiles/@cmake_cxx_std.dir/std.pcm' via by-name;
Location B: 'CMakeFiles/@cmake_cxx_std@synth_0.dir/1f708004f604.bmi' via by-name.
```

but the build proceeds and the compile then dies with a segfault rather than an
error message.

## Reproducer

`dep.cppm`
```cpp
export module dep;
#ifdef EXTRA
// Not exported and never referenced; it only perturbs the contents of the BMI
// so that the two builds of this module produce *different* module files.
inline int hidden() { return 42; }
#endif
export struct S { int v = 1; };
```

`lib.cppm`
```cpp
export module lib;
import dep;
export inline int use(S s) { return s.v; }
```

`main.cc`
```cpp
import lib;
int main() { return use({}); }
```

```sh
clang++ -std=c++23 -x c++-module dep.cppm --precompile -o dep1.pcm
clang++ -std=c++23 -DEXTRA -x c++-module dep.cppm --precompile -o dep2.pcm
clang++ -std=c++23 -x c++-module lib.cppm --precompile -fmodule-file=dep=dep1.pcm -o lib.pcm

# `lib` was built against dep1.pcm, but the consumer is handed dep2.pcm:
clang++ -std=c++23 -fmodule-file=lib=lib.pcm -fmodule-file=dep=dep2.pcm -c main.cc -o main.o
```

## Actual behaviour

The last command crashes (exit code 139, SIGSEGV):

```
1.	main.cc:2:21: current parser token 'use'
2.	main.cc:2:12: parsing function body 'main'
3.	main.cc:2:12: in compound statement ('{}')
 #5 clang::ASTContext::getTagType(clang::ElaboratedTypeKeyword, clang::NestedNameSpecifier, clang::TagDecl const*, bool) const
 #6 clang::ASTReader::readTypeRecord(unsigned long)
 #7 clang::ASTReader::GetType(unsigned long)
 #8 llvm::ArrayRef<clang::QualType> clang::serialization::DataStreamBasicReader<clang::ASTRecordReader>::readArray<clang::QualType>(llvm::SmallVectorImpl<clang::QualType>&)
 #9 clang::serialization::AbstractTypeReader<clang::ASTRecordReader>::readFunctionProtoType()
#10 clang::ASTReader::readTypeRecord(unsigned long)
#11 clang::ASTReader::GetType(unsigned long)
#12 clang::ASTDeclReader::VisitDeclaratorDecl(clang::DeclaratorDecl*)
```

Reading the type of `use` resolves `S` through the wrong module file, and
`getTagType` is reached with a null `TagDecl`.

In a larger project the same input crashes at a different point in the reader,
e.g. while deserializing an inline function body:

```
 #5 clang::ASTReader::ReadSourceLocation(clang::serialization::ModuleFile&, unsigned long) const
 #6 clang::ASTRecordReader::readCXXBaseSpecifier()
 #7 clang::ASTStmtReader::VisitCastExpr(clang::CastExpr*)
...
#11 clang::FunctionDecl::getBody() const
#12 clang::Sema::MarkFunctionReferenced(clang::SourceLocation, clang::FunctionDecl*, bool)
```

## Expected behaviour

A diagnostic identifying the inconsistent module file — the situation is
detectable, since `lib.pcm` records which `dep` BMI it was built against.
Any error is better than a segfault.

## Notes on the trigger

* Passing **no** `-fmodule-file=dep=…` to the consumer (letting `lib.pcm`'s
  recorded dependency resolve) compiles fine.
* Passing a `dep2.pcm` built from identical source with identical flags
  (byte-identical BMI) compiles fine.
* Passing a `dep2.pcm` that differs only in `-I` include paths — the BMIs differ
  byte-wise but carry the same declarations — also compiles fine.
* The crash requires the second BMI to differ in *declaration content*, and only
  fires when the consumer actually references an entity that has to be
  deserialized: replacing `main` with `int main() { return 0; }` (importing
  `lib` but naming nothing from it) compiles fine.

## Versions tested

Both crash, on `x86_64-unknown-linux-gnu`:

* clang version 22.1.8 (llvm.org release tarball `LLVM-22.1.8-Linux-X64`, `ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`)
* Ubuntu clang version 21.1.8 (1:21.1.8-6ubuntu1)
