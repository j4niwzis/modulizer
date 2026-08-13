#include "frienddef_lib/producer.h"

namespace frienddef_lib {
namespace internal {

// The consumer defines the class the library only forward-declared and
// friend-declared.
class Gen {};

Gen* make_gen() { return new Gen(); }

}  // namespace internal
}  // namespace frienddef_lib
