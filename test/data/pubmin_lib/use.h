#pragma once

#include "pubmin_lib/defs.h"

namespace pubmin_lib {
namespace internal {

template <typename T>
class Gen {
 public:
  int run() const { return Foo().value(); }
};

}  // namespace internal
}  // namespace pubmin_lib
