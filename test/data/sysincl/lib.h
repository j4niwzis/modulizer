#pragma once

#include "absl/flags/declare.h"

namespace sysincl {

class Foo {
 public:
  int v() const { return 1; }
};

}  // namespace sysincl
