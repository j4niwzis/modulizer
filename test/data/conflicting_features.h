#pragma once

namespace test_lib {

#ifdef FEATURE_A
auto a(auto) {}
#endif

#ifdef FEATURE_B
#ifdef FEATURE_A
static_assert(false, "You can't use A with B");
#endif
auto b(auto) {}
#endif

}  // namespace test_lib
