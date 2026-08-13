#pragma once

#if defined(__clang__) || defined(__GNUC__)
#define COND_HELPER(x) x * 2
#else
#define COND_HELPER(x) x
#endif

namespace macronest {

class Cond {
 public:
  int v() const { return COND_HELPER(10); }
};

}  // namespace macronest
