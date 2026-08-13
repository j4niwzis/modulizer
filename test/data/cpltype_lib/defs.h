#pragma once

namespace cpltype_lib {
namespace internal {

// A complete internal struct referenced BY VALUE by a cross-module function
// template. When the template is injected into a using module, this struct
// must be injected in full (a forward declaration would leave the by-value
// parameter incomplete).
struct Baz {};

template <typename T>
bool equal(Baz lhs, const T& rhs);

}  // namespace internal
}  // namespace cpltype_lib
