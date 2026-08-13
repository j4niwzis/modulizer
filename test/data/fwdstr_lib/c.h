#pragma once

#include <string>

namespace fwdstr_lib {

template <typename T>
std::string foo(const T& value) {
  return "defined";
}

}  // namespace fwdstr_lib
