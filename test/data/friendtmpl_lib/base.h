#pragma once

namespace friendtmpl_lib {

template <typename T>
class Bar;

class Foo {
 public:
  int value = 1;

  template <typename T>
  friend class Bar;
};

}  // namespace friendtmpl_lib
