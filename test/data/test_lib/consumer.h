#pragma once

#include "test_lib/producer.h"

namespace test_lib {

void bar() {
  foo(detail::t{});
}

}  // namespace test_lib
