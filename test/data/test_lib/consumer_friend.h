#pragma once

#include "test_lib/producer.h"

namespace test_lib {

class Baz {
  friend class detail::FriendTarget;
};

}  // namespace test_lib
