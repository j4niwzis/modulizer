#pragma once
#include "multi_a.h"
#include <string>

namespace my_lib {

class Bar {
public:
  Foo make_foo() { return {}; }
  std::string name() const { return "bar"; }
};

}  // namespace my_lib
