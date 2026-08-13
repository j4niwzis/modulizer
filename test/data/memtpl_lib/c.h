#pragma once

namespace memtpl_lib {

template <typename T>
struct Box {
  template <typename U>
  Box(U);
};

template <typename T>
template <typename U>
Box<T>::Box(U) {}

}  // namespace memtpl_lib
