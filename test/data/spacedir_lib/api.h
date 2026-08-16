#pragma once

// Directives spelled with whitespace between the `#` and the keyword, which is
// ordinary C++ and common in libraries that indent nested conditionals.
# if defined(SPACEDIR_WIN)
#   define SPACEDIR_API_KIND 1
# else
#   define SPACEDIR_API_KIND 2
# endif

namespace spacedir {
struct Handle { int v; };
}
