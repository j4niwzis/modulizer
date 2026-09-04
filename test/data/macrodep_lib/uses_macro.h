#ifndef MACRODEP_LIB_USES_MACRO_H
#define MACRODEP_LIB_USES_MACRO_H
#include <provider/thing.hpp>
#define MACRODEP_CHECK(x) ((x) ? PROVIDER_TAG : 0)
namespace macrodep_lib { inline int use(int x) { return MACRODEP_CHECK(x); } }
#endif
