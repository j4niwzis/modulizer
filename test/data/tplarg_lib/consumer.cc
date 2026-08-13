#include <type_traits>

#include "tplarg_lib/producer.h"

namespace tplarg_lib {
namespace internal {

// References `disjunction` ONLY through the template arguments of the variable
// template `std::is_base_of_v`: the AST traverses the DeclRefExpr to the
// VarTemplateSpecializationDecl, never the nested `disjunction<>` type itself.
static_assert(std::is_base_of_v<std::false_type, disjunction<>>, "");

// A class template used as a template argument to another template is also
// only reachable through the outer template's arguments.
static_assert(std::is_same_v<box_type<int>, int>, "");

}  // namespace internal
}  // namespace tplarg_lib
