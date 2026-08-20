#ifndef PRESENCERR_H_INCLUDED
#define PRESENCERR_H_INCLUDED

// A configuration header that picks the API for itself and refuses to be
// handed the choice: it reads its own macros before it defines them, purely to
// reject a definition that came from outside.
#if defined(PRESENCERR_WINDOWS_API) || defined(PRESENCERR_POSIX_API)
#  error PRESENCERR_WINDOWS_API and PRESENCERR_POSIX_API must not be defined by users
#endif

#if defined(_WIN32)
#  define PRESENCERR_WINDOWS_API
#else
#  define PRESENCERR_POSIX_API
#endif

namespace test_lib {

int api_kind();

} // namespace test_lib

#endif // PRESENCERR_H_INCLUDED
