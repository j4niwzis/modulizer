#pragma once

#include "dep_lib/umbrella.h"

namespace mylib {

class UseUmbrella {
 public:
  int v() const {
    return dep_lib::Foo().v() + dep_lib::internal::Bar().v();
  }
};

}  // namespace mylib
