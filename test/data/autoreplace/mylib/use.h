#pragma once

#include "dep_lib/dep.h"

namespace mylib {

class Use {
 public:
  int v() const { return dep_lib::Foo().v(); }
};

}  // namespace mylib
