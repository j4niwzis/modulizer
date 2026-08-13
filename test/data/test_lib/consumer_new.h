#pragma once

#include "test_lib/producer.h"

namespace test_lib {

template <typename T, typename I>
void gen_fn(T a, T b, I step) {
  new detail::Gen<T, I>(a, b);
}

}  // namespace test_lib
