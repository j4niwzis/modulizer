#pragma once

#include "other.h"

namespace crosslib {

class Box {
 public:
  int area() const { return other_lib::Foo().size() * other_lib::Foo().size(); }
};

}  // namespace crosslib
