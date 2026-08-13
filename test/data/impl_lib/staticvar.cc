#include "impl_lib/api2.h"

namespace impl_lib {

int Baz::count = 0;

int Baz::size() const { return count; }

}  // namespace impl_lib
