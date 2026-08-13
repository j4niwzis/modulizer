#pragma once

#include "test_lib/producer.h"

namespace test_lib {

template <typename T,
          typename = typename detail::Size<sizeof(detail::Flag)>::Type>
void use_sfinae(const T&) {}

}  // namespace test_lib
