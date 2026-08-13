#pragma once

namespace usinglib {

namespace internal {

class Foo {
 public:
  int size() const { return 4; }
};

}  // namespace internal

using internal::Foo;

Foo make_foo();

}  // namespace usinglib
