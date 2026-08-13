#pragma once

#if SOME_CONDITION && \
    ANOTHER_CONDITION
#include <inside_multiline.h>
#endif

// Separate guarded include
#ifdef SIMPLE_GUARD
#include <simple_guard.h>
#endif

namespace backslash_guard_test {

class Baz {};

}  // namespace backslash_guard_test
