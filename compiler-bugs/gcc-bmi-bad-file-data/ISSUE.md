# [gcc][modules] "Bad file data" reading a BMI cluster, and a second defect without `import std`

**NOT REDUCED — no runnable reproducer in this directory.** Everything else here
has one; this is kept as a field note so the next attempt does not start over.
It needs more of a module graph than any hand-written shape reproduced, so the
observations below are against the real converted tree rather than a `foo`/`bar`
reduction.

Two distinct gcc defects are recorded: the BMI read failure (most of this file),
and, at the end, an `<ostream>` lookup failure that appears instead when the
standard library is textual rather than imported.

## The failure

Building any test translation unit that uses a `Message`-constructing macro:

```
gtest.gtest_assertion_result: error: failed to read compiled module cluster 63: Bad file data
gtest.gtest_internal_inl:     error: failed to read compiled module cluster 64: Bad file data
gtest-internal-macros.h:36:26: fatal error: failed to load pendings for 'std::unique_ptr'
   36 |       ::testing::Message()
```

g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0. Deterministic: a clean build dir gives
the same clusters, and the same two modules, every time.

## Reduced trigger

One line of the test body does it. Bisected against
`googletest/test/gtest_environment_test.cc` — 23 lines of body compile, 24 fail:

```cpp
ADD_FAILURE() << "Expected non-fatal failure in global set-up.";
```

which expands to constructing `::testing::Message` — a class whose only data
member is `std::unique_ptr<::std::stringstream>` — and streaming into it.

Reduced consumer, still failing, is two imports and one line:

```cpp
import std.compat;
import gtest;
#include "gtest/gtest-macros.h"
#include "gtest-internal-inl-macros.h"
void f() { ADD_FAILURE() << "x"; }
```

Dropping the body (imports only, `int main() {}`) compiles. So it is the *use*
that fails, not the import.

## What did NOT reproduce it

Four attempts, all compiling cleanly:

1. A module exporting a class with a `std::unique_ptr<std::stringstream>`
   member, consumed by another TU.
2. The same plus a template `operator<<`, an `operator=` taking it, and a
   re-exporting umbrella module — the shape `ADD_FAILURE()` expands to.
3. Two modules each holding a `std::unique_ptr` of a different type, both
   re-exported through one umbrella, to see if several modules contributing
   pendings for the same `std` entity was the trigger.
4. A real three-module subset of the tree (`gtest-message`, `gtest-port`,
   `gtest-port-arch`), converted the same way and built by hand with g++, with
   a consumer that constructs `Message`.

So it needs more of the module graph than any of these. The next thing to try
is bisecting the *module set*: convert progressively larger subsets until the
smallest failing one is found.

## `import std` is not the cause

Converting without `--import-std` (stdlib headers textually in the global
module fragment) and building the same way fails EARLIER, at 82/280, on a
different gcc defect:

```
gtest-param-util.h:395:  << "from different generators." << std::endl;
/usr/include/c++/15/ostream:67:19: error: 'flush' was not declared in this
    scope; did you mean 'fflush'?
```

So both configurations hit gcc module defects; the `import std` one gets
further. Turning `import std` off is not a workaround.
