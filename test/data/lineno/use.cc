// Copyright notice, line 1.
// Line 2.
// Line 3.

#include "lineno/lib.h"
#include <cerrno>

int main()
{
    return lineno::value() == 0 ? 0 : 1;   // LINENO_MARKER
}
