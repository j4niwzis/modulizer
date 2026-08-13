#include "fwdonly_lib/producer.h"

namespace fwdonly_lib {
namespace internal {

// The consumer provides the definition the library only forward-declared.
class Foo {};

Foo* make_foo() { return new Foo(); }

}  // namespace internal
}  // namespace fwdonly_lib
