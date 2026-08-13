#pragma once

namespace friendimpl_lib {

class Foo {
public:
  friend class Bar;
  int value = 1;
};

}  // namespace friendimpl_lib
