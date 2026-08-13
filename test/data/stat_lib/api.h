#pragma once

namespace stat_lib {

template <typename T>
static void Delete(T* x) {
  delete x;
}

}  // namespace stat_lib
