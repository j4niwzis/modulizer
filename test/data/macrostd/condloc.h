#pragma once

// A standard header first, exactly as the real case has one: it is what
// defines the feature-test macro the guard below asks about. Without it the
// guarded branch is dead, and a dead branch needs nothing.
#include <string>

// The header the macro body needs is behind the same feature test the macro
// definition is behind. The two stand or fall together, so the include has to
// travel with its condition rather than be dropped for having one.
#if defined(__cpp_lib_source_location) && __cpp_lib_source_location >= 201907L
#include <source_location>
#endif

#if defined(__cpp_lib_source_location) && __cpp_lib_source_location >= 201907L
#define MACROSTD_CONDHERE ::std::source_location::current()
#else
#define MACROSTD_CONDHERE 0
#endif

namespace macrostd {

int cond_line_of();

} // namespace macrostd
