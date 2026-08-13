#pragma once

namespace consdef_lib {
namespace internal {

// Forward-declared in the library; a consumer defines the class itself.
class Helper;

// Friend-declares `Helper`: any consumer that includes this header (even one
// that does NOT define `Helper`) references it through the friend declaration.
class Widget {
 public:
  friend class internal::Helper;
  int size() const { return 42; }
};

}  // namespace internal
}  // namespace consdef_lib
