#pragma once

#include "test_lib/producer.h"

namespace test_lib {

template <typename T>
struct AliasUser;

template <typename T>
struct AliasUser<detail::Baz<T>> {};

}  // namespace test_lib
