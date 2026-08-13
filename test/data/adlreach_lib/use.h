#pragma once

#include "adlreach_lib/prod.h"

namespace adlreach_lib {
namespace internal {
template <typename F>
struct Gen {
  bool foo(const F& f) {
    return sizeof(Foo<F>) > 0;
  }
};
}
}

#define ADLREACH_USE(obj, arg) \
  (::adlreach_lib::internal::Gen<decltype((arg))>{}.foo((arg)))
