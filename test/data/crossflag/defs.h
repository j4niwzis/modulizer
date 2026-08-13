#pragma once

namespace crossflag {

// A flag-declaration macro: expands to an `extern` variable in the namespace.
// Unlike MATCHER-style macros (which declare classes/functions that must carry
// the export wherever they expand), a shared DECLARE macro is expanded in BOTH
// interface units (exported) and implementation units (where `export` is
// ill-formed). The export marker must stay at the invocation, never be baked
// into the macro body — baking it would leak `export` into every expansion.
#define DECLARE_FLAG(name) extern bool name;

}  // namespace crossflag
