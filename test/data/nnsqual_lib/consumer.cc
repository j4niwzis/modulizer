#include "nnsqual_lib/producer.h"

namespace nnsqual_lib {
namespace internal {

// The consumer provides the definition the library only forward-declared.
class Helper {
 public:
  static int go() { return 42; }
};

}  // namespace internal
}  // namespace nnsqual_lib

// File-scope using-declaration (the `using
// mylib::internal::Accessor;` pattern).
using nnsqual_lib::internal::Helper;

namespace my_namespace {
namespace testing {

// The class is referenced ONLY through a qualified member access from a nested
// namespace — it appears solely as the nested-name-specifier qualifier type.
int run() { return Helper::go(); }

}  // namespace testing
}  // namespace my_namespace
