#pragma once

namespace friendlib {
namespace internal {
template <class T>
class Helper {};
}  // namespace internal

class Public {
 public:
  // A class-scope typedef must NOT be re-exported as `friendlib::size_t`.
  typedef int size_t;
  int x;
  template <class T>
  friend class internal::Helper;
};
}  // namespace friendlib
