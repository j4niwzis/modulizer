// Copyright notice, the way a real header opens: the guard is not the
// first line of the file, only the first directive in it.

#ifndef FRAGGUARD_FACET_HPP
#define FRAGGUARD_FACET_HPP

#include "fragguard/config.hpp"

#define FRAGGUARD_BEGIN_NAMESPACE \
    namespace fragguard { \
    namespace detail {

#define FRAGGUARD_END_NAMESPACE \
    } \
    }
#define FRAGGUARD_DECL FRAGGUARD_CONFIG_DECL

#include "fragguard/body.hpp"

#undef FRAGGUARD_BEGIN_NAMESPACE
#undef FRAGGUARD_END_NAMESPACE
#undef FRAGGUARD_DECL

#endif // FRAGGUARD_FACET_HPP
