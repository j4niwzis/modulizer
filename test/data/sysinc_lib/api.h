#pragma once

// A library header that pulls in a system header from an `#elif` branch which
// the implementation file uses directly.
#ifndef SYSINC_LIB_USE_REGEX
#define SYSINC_LIB_USE_REGEX 1
#endif

#ifndef SYSINC_LIB_USE_RE2
#define SYSINC_LIB_USE_RE2 0
#endif

#if SYSINC_LIB_USE_RE2
#include <sys/types.h>
#elif SYSINC_LIB_USE_REGEX
#include <regex.h>
#endif

namespace sysinc_lib {

class Foo {
public:
  bool foo(const char *text) const;
};

}  // namespace sysinc_lib
