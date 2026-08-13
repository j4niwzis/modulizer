#pragma once

namespace adlreach_lib {
namespace internal {
enum class Kind { kA, kB };

template <Kind k, typename T>
using Box = T;

template <typename T>
using Foo = Box<Kind::kB, T>;
}
}
