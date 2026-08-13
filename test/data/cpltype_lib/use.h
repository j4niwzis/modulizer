#pragma once

#include "cpltype_lib/defs.h"

namespace cpltype_lib {
namespace internal {

template <typename T>
class Gen {
 public:
  bool run(Baz lhs, const T& rhs) const { return equal(lhs, rhs); }
};

}  // namespace internal
}  // namespace cpltype_lib
