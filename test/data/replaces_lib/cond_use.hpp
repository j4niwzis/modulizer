#pragma once

// Stands in for the provider's own config header. In a real tree this macro
// comes from the provider, which is the whole difficulty: the condition below
// cannot be read until the provider's macros are in hand.
#define PROVIDER_WORKAROUND(x) (x)

#if PROVIDER_WORKAROUND(1)
#include <provider/thing.hpp>
#endif

namespace test_lib {

int cond_make_thing(int v);

} // namespace test_lib
