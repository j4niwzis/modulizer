#pragma once

#include "friend_lib/fwd.h"

namespace friend_lib {

class Bar {
public:
  friend class Foo;
  int value = 7;
};

class Foo {
public:
  void watch(const Bar &env) { (void)env; }
};

}  // namespace friend_lib
