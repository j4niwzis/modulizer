#pragma once

#include "other.h"
#include "internal/helper.h"

namespace replacelib {

class Box {
 public:
  int area() const { return other_lib::Foo().size(); }
  int help() const { return other_lib::internal::Bar().val(); }
};

}  // namespace replacelib
