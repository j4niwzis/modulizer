#pragma once

namespace my_lib {

class Foo {
public:
  int size() const { return 42; }
};

inline void foo(int x) { (void)x; }

}  // namespace my_lib
