#pragma once

#include "fwd_lib/l.h"

namespace fwd_lib {

class Baz {
public:
  int size() const { return 42; }
};

Baz* get_baz();

}  // namespace fwd_lib
