#pragma once

namespace test_lib {

#define PUBLIC_HELPER detail::foo()
#define CONVERT(x) detail::convert_##x(0)
#define CHAIN detail::intermediate

namespace detail {
  struct Foo { void go(); };
  inline Foo foo() { return {}; }
  inline int convert_foo(int) { return 0; }
  inline int convert_bar(int) { return 0; }
  inline void intermediate() {}
}

class Foo {
public:
  void method();
};

inline int foo(int x) { return x * 2; }

enum class Baz { Red, Green, Blue };

using Size = int;

constexpr int VERSION = 1;

}  // namespace test_lib
