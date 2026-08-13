#pragma once

#include <type_traits>

namespace tplarg_lib {
namespace internal {

// A variadic class template referenced by consumers ONLY through the template
// arguments of a variable template (`std::is_base_of_v<..., disjunction<>>`).
template <typename...>
struct disjunction : std::false_type {};

template <typename T>
using box_type = T;

}  // namespace internal
}  // namespace tplarg_lib
