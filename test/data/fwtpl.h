#pragma once

#include <string>

namespace fwtpl_lib {

// Forward-declared function template (no body): must be complete=false so
// cross_module_fwd_declared_fqns flags it.
template <typename T>
std::string foo(const T& value);

// Defined function template (has a body): must stay complete=true.
template <typename T>
std::string bar(const T& value) {
  return std::string("bar");
}

}  // namespace fwtpl_lib
