#pragma once

#include "tpl_lib/a.h"

namespace tpl_lib {

template <typename T>
struct Foo {};

template <>
struct Foo<int> {};

}  // namespace tpl_lib
