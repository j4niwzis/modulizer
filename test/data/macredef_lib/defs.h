#pragma once

// A macro whose value depends on a feature-test macro. The macros file is
// written before the standard library is, so it computes this against the
// wrong answer; the header, reaching it later, computes the right one.
#if defined(MACREDEF_FEATURE)
#define MACREDEF_VALUE 1
#else
#define MACREDEF_VALUE 0
#endif

namespace macredef_lib {
inline int value() { return MACREDEF_VALUE; }
}  // namespace macredef_lib
