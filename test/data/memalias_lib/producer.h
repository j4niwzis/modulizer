#pragma once

#include <type_traits>

namespace memalias_lib {
namespace internal {

template <typename Void, typename T>
struct foo_impl : std::true_type {};

// Alias template referenced by a consumer only through a dependent MEMBER
// access (`foo<int>::value`), which the AST sees as a member access whose
// base type is the alias specialization.
template <typename T>
using foo = foo_impl<void, T>;

}  // namespace internal
}  // namespace memalias_lib
