#pragma once

// A library header that pulls in C/POSIX headers textually. After module
// conversion the consumer must include them itself: `import std.compat;`
// cannot provide the C library's macros (errno, assert, NULL) or POSIX
// declarations/types (ssize_t, pthread_*).

#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

namespace sysuse_lib {

namespace internal {

// The internal failure class a library macro expands to; consumers using the
// macro must import the module that exports it.
class Failure {};

}  // namespace internal

// Expands to code referencing the internal class and calling `fflush(stderr)`.
#define FATAL() (fflush(stderr), ::sysuse_lib::internal::Failure())

class Foo {
public:
  void foo() const;
};

}  // namespace sysuse_lib
