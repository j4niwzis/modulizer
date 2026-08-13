#pragma once

// Active on this platform
#define ACTIVE_MACRO 1

// Inactive #if branch — this macro should still be extracted textually
#if 0
#define INACTIVE_PLATFORM_MACRO 2
#endif

// Guarded but not taken
#ifdef NEVER_DEFINED
#define GUARDED_DEFINE 3
#endif

namespace textual_macro_test {

class Foo {};

}  // namespace textual_macro_test
