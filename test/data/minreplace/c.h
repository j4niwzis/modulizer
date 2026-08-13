#pragma once

#include "a.h"
#include "b.h"

namespace minlib {

class C {
 public:
  int v() const { return A().v() + B().v(); }
};

}  // namespace minlib
