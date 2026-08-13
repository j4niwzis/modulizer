#pragma once

#include "test_lib/producer.h"

namespace test_lib {

void out_fn() {
  detail::Bar<int>::Out(7);
}

}  // namespace test_lib
