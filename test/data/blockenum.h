#pragma once

namespace blockenum_lib {

template <class T>
inline int size_of() {
  enum { count = sizeof(T) };
  return count;
}

}  // namespace blockenum_lib
