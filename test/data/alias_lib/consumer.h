#pragma once

#include "alias_lib/producer.h"

namespace alias_lib {

template <typename... T>
struct Box {
  typename internal::Foo<sizeof...(T)>::type shade;
};

}  // namespace alias_lib
