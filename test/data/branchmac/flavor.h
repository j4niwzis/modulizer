#ifndef BRANCHMAC_FLAVOR_H_INCLUDED
#define BRANCHMAC_FLAVOR_H_INCLUDED

#define BRANCHMAC_CLANG 0
#define BRANCHMAC_INTEL 0
#define BRANCHMAC_GCC 0
#define BRANCHMAC_MSVC 0
#define BRANCHMAC_WIDTH 64

// A compiler chain, as a configuration header writes one: an arm per compiler,
// each holding definitions of its own and conditions written in its own terms.
// The arms nest conditionals at their own indent and at column 0, and one
// condition is continued across lines. Only one arm is ever taken.
#if defined(__clang__)

# undef BRANCHMAC_CLANG
# define BRANCHMAC_CLANG 1

# if defined(__has_cpp_attribute)
#  if __has_cpp_attribute(fallthrough)
#   define BRANCHMAC_HAS_ATTR 1
#  endif
# endif

#if BRANCHMAC_CLANG < 400 && __cplusplus >= 201402L \
   && defined( __GLIBCXX__ ) && !__has_include(<shared_mutex>)

// The block holds a declaration rather than a macro, and its condition asks
// __has_include with an angle-bracketed name.

   extern "C" char *branchmac_gets (char *__s);
#endif

#elif defined(__INTEL_COMPILER)

# undef BRANCHMAC_INTEL
# define BRANCHMAC_INTEL 1

#elif defined(__GNUC__)

# undef BRANCHMAC_GCC
# define BRANCHMAC_GCC 1

#elif defined(_MSC_VER)

# undef BRANCHMAC_MSVC
# define BRANCHMAC_MSVC 1

# if BRANCHMAC_MSVC < 1920
#  define BRANCHMAC_NO_CONSTEXPR
# endif

#if BRANCHMAC_MSC_FULL < 190024210
#  undef BRANCHMAC_WIDTH
#  define BRANCHMAC_WIDTH 32
#endif

#endif

namespace branchmac {

int width();

}  // namespace branchmac

#endif  // BRANCHMAC_FLAVOR_H_INCLUDED
