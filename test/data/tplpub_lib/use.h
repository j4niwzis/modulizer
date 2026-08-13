#pragma once

#include <string>

#include "tplpub_lib/defs.h"

#define USE_FOO_IMPL(expr) ::tplpub_lib::foo(expr)

namespace tplpub_lib {

template <typename T>
class Gen {
 public:
  static std::string run(const T& t) { return USE_FOO_IMPL(t); }
};

// The definition comes AFTER the reference above, mirroring how
// a library declares a helper in its internal header, uses it in
// macro-expanded code, and only defines it later in the same header.
template <typename T>
std::string foo(const T& value) {
  return std::string();
}

}  // namespace tplpub_lib
