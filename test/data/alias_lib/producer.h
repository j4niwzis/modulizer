#pragma once

namespace alias_lib {
namespace internal {

struct Baz {
  int value;
};

template <int N>
struct Bar {
  using type = Baz;
};

template <int N>
using Foo = typename Bar<N>::type;

}  // namespace internal
}  // namespace alias_lib
