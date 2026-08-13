#pragma once

#if defined(__clang__) || defined(__GNUC__)
#define NESTED_HELPER(x) x * 2
#else
#define NESTED_HELPER(x) x
#endif

namespace macronest {

class Foo {
 public:
  int v() const { return NESTED_HELPER(21); }
};

}  // namespace macronest
