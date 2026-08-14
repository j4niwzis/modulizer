#pragma once

#include "consumerhdr_lib/producer.h"

// A consumer's own header: not one of the headers being rewritten, so what it
// writes is consumer code.
namespace helper {

template <typename T>
struct Holder {
  using Arg = typename consumerhdr_lib::internal::Elem<T>::type;
};

}  // namespace helper
