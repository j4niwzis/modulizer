#pragma once

#include <source_location>

// The macro body names a standard-library type. Wherever this macro is used,
// that header has to have been read — and the macros file is the only thing
// that travels with the macro.
#define MACROSTD_HERE ::std::source_location::current()

namespace macrostd {

int line_of();

} // namespace macrostd
