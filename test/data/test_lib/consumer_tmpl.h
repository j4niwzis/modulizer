#pragma once

#include "test_lib/producer.h"

namespace test_lib {

template <typename T>
struct Tmpl {
  typedef typename detail::Size<sizeof(T)>::Type SizeType;
  void check() {
    foo(detail::t{});
  }
};

}  // namespace test_lib
