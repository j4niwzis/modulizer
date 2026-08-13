#include "consdef_lib/producer.h"

namespace consdef_lib {
namespace internal {

// This consumer DEFINES the class the library only forward-declared. Exporting
// the module's forward declaration would collide with this global-module
// definition.

class Helper {
 public:
  static int go();
};

int call_helper() { return Helper::go(); }

}  // namespace internal
}  // namespace consdef_lib
