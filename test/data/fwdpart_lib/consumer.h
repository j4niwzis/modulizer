#pragma once

#include "fwdpart_lib/producer.h"

namespace fwdpart_lib {

template <typename F>
void use_box() {
  Foo<F> r = {};
  (void)r;
}

}  // namespace fwdpart_lib
