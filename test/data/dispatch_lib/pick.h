#pragma once

// One implementation of `thing`, never two. The indented `# include` spelling
// is what libraries write inside conditionals.
#if defined(DISPATCH_LIB_USE_B)
# include "dispatch_lib/impl_b.h"
#else
# include "dispatch_lib/impl_a.h"
#endif

namespace dispatch_lib {

inline int pick() { return thing().value(); }

}  // namespace dispatch_lib
