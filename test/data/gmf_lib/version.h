#pragma once

namespace gmf_lib {

// `__cpp_lib_three_way_comparison` is defined by <version>, but this include
// is guarded by a macro whose VALUE depends on that feature-test macro — the
// GMF macros file must be processed AFTER <version> so the guard resolves
// correctly (the feature-test-macro regression: without
// `<version>` the `#if GMFLIB_HAS_VERSION` block in the module
// body is compiled out).
#if defined(__cpp_lib_three_way_comparison)
#define GMFLIB_HAS_VERSION 1
#else
#define GMFLIB_HAS_VERSION 0
#endif

#if GMFLIB_HAS_VERSION
#include <version>
#endif

class VersionBox {
 public:
  int size() const { return 1; }
};

}  // namespace gmf_lib
