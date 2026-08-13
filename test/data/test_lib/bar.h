#pragma once

#include "test_lib/internal/foo.h"

namespace test_lib {

inline int bar() {
  detail::Foo f;
  return f.value();
}

}  // namespace test_lib
