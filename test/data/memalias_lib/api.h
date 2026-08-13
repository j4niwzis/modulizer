#pragma once

namespace memalias_lib {

template <typename T>
class Foo {
 public:
  // Member alias template inside a class — must NOT be exported.
  template <typename U>
  using Bar = U;

  using Baz = int;
};

}  // namespace memalias_lib
