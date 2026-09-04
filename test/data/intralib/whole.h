#pragma once

// A sibling of this same library. It becomes a module of its own, so a
// consumer may have imported it before reading this header — reached, say,
// through a third-party header that was never converted.
#include "intralib/part.h"

namespace intralib {

int use(const Part &p);

}  // namespace intralib
