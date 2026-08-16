#pragma once

namespace splitovl_lib {
namespace detail {

class Entry;

// An overload defined right here.
inline int dispatch(int value) { return value; }

// Another overload of the same name, defined in entry.h to break a circular
// header dependency. The declaring header defining an overload of its own does
// not make this pair local.
int dispatch(Entry const& entry);

}  // namespace detail
}  // namespace splitovl_lib
