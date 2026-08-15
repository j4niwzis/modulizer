#pragma once

#include "injextern_lib/defs.h"

namespace injextern_lib {
namespace internal {

template <class T>
class Gen {
 public:
  static T take() { return Make<T>(nullptr); }
  static bool ok(int n) { return Check(n); }
};

}  // namespace internal
}  // namespace injextern_lib
