#include "consdef_lib/producer.h"

// This consumer references the library but does NOT define `Helper` itself.
// Its trace must still record `Helper` (referenced via the friend declaration),
// but the global consumer-defined filter below must drop it.

namespace consdef_lib {
namespace internal {

int use_widget() { return Widget().size(); }

}  // namespace internal
}  // namespace consdef_lib
