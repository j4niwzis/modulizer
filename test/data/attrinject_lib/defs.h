#pragma once

#include <type_traits>

#define TEST_API_ __attribute__((visibility("default")))

namespace attrinject_lib {
namespace internal {

template <class T>
class TEST_API_ [[nodiscard]] Box {
 public:
  typedef T type;
  static T make();
};

template <typename T>
class TEST_API_ [[nodiscard]] Holder : public Box<T> {
 public:
  typedef typename Box<T>::type value_type;
};

template <typename P>
struct Baz : std::integral_constant<bool, bool(P::value)> {};

}  // namespace internal
}  // namespace attrinject_lib
