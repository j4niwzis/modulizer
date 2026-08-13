#pragma once

namespace wraplib {
namespace internal {
class Helper;
void helper_func();
template <typename T>
void templated(T t);
template <typename T>
class Box {
 public:
  using type = T;
};
}  // namespace internal

namespace adl_guard {
struct Baz {};
}  // namespace adl_guard

using namespace adl_guard;

class Public {
 public:
  int x;
};
}  // namespace wraplib
