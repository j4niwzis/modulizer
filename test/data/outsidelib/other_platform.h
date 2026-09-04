#ifndef OUTSIDELIB_OTHER_PLATFORM_H
#define OUTSIDELIB_OTHER_PLATFORM_H

// A header for a platform this is not. Nothing includes it here — the headers
// that do are behind the same test — so parsing it on its own says nothing
// about what a consumer needs, and it cannot even be read to the end.
#if !defined(OUTSIDELIB_OTHER_PLATFORM)
#  error this header is for another platform
#endif

#include <outside/platform_only.h>

namespace outsidelib {
outside::PlatformOnly platform_thing();
}

#endif // OUTSIDELIB_OTHER_PLATFORM_H
