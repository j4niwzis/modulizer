#pragma once

#include "a.h"

namespace tranlib {

class W {
 public:
  int v() const { return A().v() + B().v(); }
};

}  // namespace tranlib
