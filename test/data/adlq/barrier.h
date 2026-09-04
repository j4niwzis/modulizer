#ifndef ADLQ_BARRIER_H
#define ADLQ_BARRIER_H

namespace adlq {
namespace inner {

// The ADL barrier: what it holds is reached through using-directives, not by
// naming this namespace.
namespace barrier {

inline int thing(int v) { return v; }

}  // namespace barrier

// Reached from the namespace that contains the barrier: the short name works.
using namespace barrier;

}  // namespace inner

// And from one further out, where the short name is not a thing that can be
// named. (Boost.Iterator's advance.hpp does exactly this.)
using namespace inner::barrier;

}  // namespace adlq

#endif  // ADLQ_BARRIER_H
