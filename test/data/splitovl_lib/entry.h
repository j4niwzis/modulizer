#pragma once

#include "splitovl_lib/traits.h"

namespace splitovl_lib {
namespace detail {

class Entry {
 public:
  int value() const { return 7; }
};

inline int dispatch(Entry const& entry) { return entry.value(); }

}  // namespace detail
}  // namespace splitovl_lib
