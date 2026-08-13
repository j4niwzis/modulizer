#pragma once

namespace test_lib {
namespace detail {
class Foo {
public:
  static bool foo(int, int);
};
}
#define BAR(a, b) ::test_lib::detail::Foo::foo(a, b)
#define BAZ(a, b) BAR(a, b)
}  // namespace test_lib
