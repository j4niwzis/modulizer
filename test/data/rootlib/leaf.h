#pragma once

#include "rootlib/rootlib.h"

namespace rootlib {

class Leaf {
public:
  Root make() const { return Root{}; }
};

}  // namespace rootlib
