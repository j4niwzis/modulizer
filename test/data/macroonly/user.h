#pragma once

#include "macroonly/friend_macro.h"

namespace macroonly {

class Guarded {
  MACROONLY_BEFRIEND(GuardedTest, ReachesPrivateState);

  int state_ = 0;
};

} // namespace macroonly
