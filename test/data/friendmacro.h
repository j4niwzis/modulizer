#ifndef FRIENDMACRO_H
#define FRIENDMACRO_H

// One macro, expanded in two places: inside the class as a friend declaration,
// and at namespace scope as the definition. Two separate declarations that
// spell the same place -- the macro's body -- so a marker written there for
// the second reaches the first, where neither `export` nor `extern "C++"` is
// a thing that can be written.
// (Boost.Iterator's BOOST_ITERATOR_FACADE_PLUS_HEAD is the case this stands
// for: a friend at iterator_facade.hpp:487 and the definition at :866.)
#define FRIENDMACRO_PLUS_HEAD(prefix, args) \
    template <typename U>       \
    prefix U plus_head args

namespace friendmacro {

template <typename T>
class widget {
public:
  T value() const { return v_; }
  FRIENDMACRO_PLUS_HEAD(friend inline, (widget<U> const& lhs, widget<U> const& rhs));
private:
  T v_ = T();
};

FRIENDMACRO_PLUS_HEAD(inline, (widget<U> const& lhs, widget<U> const& rhs))
{ return lhs.value() + rhs.value(); }

}  // namespace friendmacro

#endif  // FRIENDMACRO_H
