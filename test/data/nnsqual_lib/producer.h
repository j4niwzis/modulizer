#pragma once

namespace nnsqual_lib {
namespace internal {

// Forward-declared in the library (the forward-declared-class pattern); the
// consumer defines the class itself and references it only through qualified
// member access.
class Helper;

// Friend-declares the same class (the friend-declared-accessor pattern).
class Widget {
 public:
  friend class internal::Helper;
};

int call_helper(Helper* h);

}  // namespace internal
}  // namespace nnsqual_lib
