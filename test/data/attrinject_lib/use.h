#pragma once

#include "attrinject_lib/defs.h"

namespace attrinject_lib {
namespace internal {

template <typename C>
class Gen {
 public:
  static typename Box<C>::type peek() { return Box<C>::make(); }

  static typename Holder<C>::value_type hold() {
    return typename Holder<C>::value_type();
  }

  template <typename T>
  static bool not_void() {
    return Baz<std::is_void<T>>::value;
  }
};

}  // namespace internal
}  // namespace attrinject_lib
