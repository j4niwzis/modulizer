// A fragment belonging to another project: read between the terms its includer
// defines, and never converted. It brings in what it needs for itself, and
// wherever it is read, this include is read too -- including inside a module
// purview, which is not where it belongs.
#include <bitset>

FRAGSTD_OPEN

class FRAGSTD_DECL codec {
public:
  std::bitset<8> mask() const;
};

FRAGSTD_CLOSE
