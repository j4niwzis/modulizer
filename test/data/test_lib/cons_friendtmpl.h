#pragma once

#include "test_lib/prod_friendtmpl.h"

namespace test_lib {

template <class T>
class Baz {
 public:
  template <class U>
  friend class detail::Bar;
};

}  // namespace test_lib
