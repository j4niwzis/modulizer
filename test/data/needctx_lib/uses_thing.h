#pragma once

// Not self-contained on purpose: `thing` is named by value, but the header
// that defines it is never included — whoever includes this one includes it
// first. Alone, the return type is incomplete.
namespace needctx_lib {

struct thing;

inline int call(thing t) { return t.value(); }

}  // namespace needctx_lib
