#pragma once

namespace macoverride_lib { struct Thing { int v; }; }

// Default definitions for all compilers, overridden for one of them by the
// `#undef` + redefine idiom. The override is one-sided: every other compiler
// keeps the definitions above it.
#define MACOVERRIDE_WARN_PUSH()
#define MACOVERRIDE_WARN(Level, Name)

#if defined(__clang__)
#undef MACOVERRIDE_WARN_PUSH
#define MACOVERRIDE_WARN_PUSH() _Pragma("clang diagnostic push")
#undef MACOVERRIDE_WARN
#define MACOVERRIDE_WARN(Level, Warning) _Pragma("clang diagnostic ignored")
#endif

#define MACOVERRIDE_MAKE(name)                              \
  MACOVERRIDE_WARN_PUSH() MACOVERRIDE_WARN(ignored, "-Wall") \
  inline ::macoverride_lib::Thing name() { return {}; }
