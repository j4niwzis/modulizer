#pragma once

#include "macronest/dep.h"

namespace macronest {

class Baz {
 public:
  int v() const { return Foo().v() + NESTED_HELPER(1); }
};

}  // namespace macronest
