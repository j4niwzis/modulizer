#pragma once

#include "tplbase_lib/defs.h"

namespace tplbase_lib {
namespace internal {
template <typename F>
class Gen {
 public:
  explicit Gen(F f) : f_(f) {}

  template <typename Tuple>
  auto call(Tuple&& args) const {
    return Foo(f_, std::forward<Tuple>(args));
  }

  template <typename T>
  bool not_void() {
    return Baz<std::is_void<T>>::value;
  }

  template <typename C>
  static typename Box<C>::const_reference view_of(const C& c) {
    return Box<C>::ConstReference(c);
  }

 private:
  F f_;
};
}
}
