#pragma once

namespace reachmod_lib {
namespace internal {

template <typename T>
using SharedAlias = T;

class Bar {
 public:
  int value() const { return 42; }
};

}  // namespace internal
}  // namespace reachmod_lib
