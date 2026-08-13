#pragma once

#include "fwdopaque/dep.h"

namespace fwdopaque {

void operator<<(const Foo&, int);

class Bar {
 public:
  int v() const { return 1; }
};

}  // namespace fwdopaque
