#pragma once

#ifdef FEATURE_A
auto a() { return 0; }
#endif
#ifdef FEATURE_B
struct a {};
a b(auto) {}
#endif
