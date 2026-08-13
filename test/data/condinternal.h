#pragma once

#include <cstdlib>

namespace test_lib {
namespace internal {
namespace posix {

#ifdef LIB_OS_A
struct Foo {
  int a;
};
#elif defined(LIB_OS_B)
struct Foo {
  int b;
};
#else
struct Foo {
  int mode;
};
#endif

#ifdef LIB_OS_A
[[noreturn]] void Bar();
#else
[[noreturn]] inline void Bar() { abort(); }
#endif

}  // namespace posix
}  // namespace internal
}  // namespace test_lib
