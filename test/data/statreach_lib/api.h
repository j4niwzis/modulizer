#pragma once

namespace statreach_lib {

template <typename T>
static void Delete(T* x) {
  delete x;
}

}  // namespace statreach_lib
