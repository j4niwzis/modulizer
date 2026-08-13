#pragma once

#include "friend_lib/alpha.h"

namespace friend_lib {

class A {
public:
  friend class B;
  int value = 1;
};

class B {
public:
  void touch(const A &a) { (void)a; }
};

}  // namespace friend_lib
