#ifndef FRAGGUARD_FACET_HPP
#define FRAGGUARD_FACET_HPP

// Nothing here but directives: the declarations come from the fragment.
#define FRAGGUARD_BEGIN_NAMESPACE namespace fragguard { namespace detail {
#define FRAGGUARD_END_NAMESPACE } }

#include "fragguard/body.hpp"

#undef FRAGGUARD_BEGIN_NAMESPACE
#undef FRAGGUARD_END_NAMESPACE

#endif  // FRAGGUARD_FACET_HPP
