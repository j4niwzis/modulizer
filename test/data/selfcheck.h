#ifndef SELFCHECK_H
#define SELFCHECK_H

// The library rejects this macro as user input, and then derives it itself.
#if defined(SELFCHECK_API)
#error SELFCHECK_API must not be defined by users
#endif

#if defined(SELFCHECK_WINDOWS)
#define SELFCHECK_API 1
#else
#define SELFCHECK_API 2
#endif

namespace selfcheck {

inline int api() { return SELFCHECK_API; }

}  // namespace selfcheck

#endif  // SELFCHECK_H
