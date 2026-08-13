#pragma once

#define GMFLIB_HAS_COMPARE_LIB 1

namespace gmf_lib {

class OrderBox {
 public:
  int size() const { return 1; }
};

// `__cpp_lib_three_way_comparison` is defined by <compare>, but this include is
// guarded by a macro whose VALUE depends on that feature-test macro — the GMF
// macros file must be processed AFTER <compare> so the guard resolves
// correctly (the feature-test-macro regression).
#if GMFLIB_HAS_COMPARE_LIB
#include <compare>
#endif

}  // namespace gmf_lib
