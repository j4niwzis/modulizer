#pragma once

#include "test_lib/bar.h"

namespace test_lib {

inline int baz() {
  detail::Foo f;
  return bar() + f.value();
}

}  // namespace test_lib
