#pragma once

#include "b.h"

namespace tranlib {

class A {
 public:
  int v() const { return B().v(); }
};

}  // namespace tranlib
