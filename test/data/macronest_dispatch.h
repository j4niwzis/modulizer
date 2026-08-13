#pragma once

namespace test_lib {
namespace detail {
class Foo {
 public:
  static bool foo(int, int);
};
}

#define MACRONEST_CAT(a, b) MACRONEST_INTERNAL_CAT(a, b)
#define MACRONEST_INTERNAL_CAT(a, b) a##b
#define MACRONEST_NARG(...) MACRONEST_NARG_IMPL(__VA_ARGS__, 1)
#define MACRONEST_NARG_IMPL(_1, _N, ...) _N
#define MACRONEST_VARIADIC_CALL(macro, ...) \
  MACRONEST_CAT(macro, MACRONEST_NARG(__VA_ARGS__))(__VA_ARGS__)

#define MACRONEST_DISPATCH_1(ret, name, args) \
  ::test_lib::detail::Foo::foo(1, 2)
#define MACRONEST_DISPATCH_2(ret, name, args, spec) \
  ::test_lib::detail::Foo::foo(1, 2)

#define MACRONEST_METHOD(...) \
  MACRONEST_VARIADIC_CALL(MACRONEST_DISPATCH_, __VA_ARGS__)

}  // namespace test_lib
