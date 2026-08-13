#pragma once

namespace frienddef_lib {
namespace internal {

class Gen;

class Baz {
 public:
  friend class internal::Gen;
};

}  // namespace internal
}  // namespace frienddef_lib
