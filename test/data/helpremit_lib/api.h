#pragma once

#include <stdio.h>

namespace helpremit_lib {

// A static helper defined in a header. Consumers (impl units) use it, so it
// must be re-homed in the global module: `static` removed, exported.
static void foo() { fflush(nullptr); }

class Foo {
public:
  void run();
};

}  // namespace helpremit_lib
