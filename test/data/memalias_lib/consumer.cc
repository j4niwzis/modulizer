#include <type_traits>

#include "memalias_lib/producer.h"

namespace memalias_lib {
namespace internal {

// References `foo` (an alias template) ONLY through a dependent member access:
// `foo<int>::value` is a MemberExpr whose base type is the alias
// specialization `foo<int>`. The tracer must check the member access base type
// to export `foo` (and its underlying `foo_impl`).
static_assert(foo<int>::value, "");

}  // namespace internal
}  // namespace memalias_lib
