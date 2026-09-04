// A fragment belonging to another project: read between the terms its includer
// defines, and never converted. It brings in what it needs for itself, and
// wherever it is read, this include is read too -- including inside a module
// purview, which is not where it belongs.
#include <bitset>

// And one that belongs to a platform this build is not. The condition is the
// fragment's own and does not travel, so hoisting this would write it for
// every platform and the build would stop on a header that is not there.
#if defined(FRAGSTD_SOME_OTHER_PLATFORM)
#include <fragstd_platform/only_there.h>
#endif

FRAGSTD_OPEN

class FRAGSTD_DECL codec {
public:
  std::bitset<8> mask() const;
};

FRAGSTD_CLOSE
