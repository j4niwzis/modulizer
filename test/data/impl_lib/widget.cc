#include "impl_lib/api3.h"

namespace impl_lib {

class Bar {
public:
  int value() const { return 42; }
};

int foo() { return Bar().value(); }

}  // namespace impl_lib
