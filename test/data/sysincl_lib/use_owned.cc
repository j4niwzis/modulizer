#include "sysincl_lib/prod.h"

// A consumer whose only mention of a system type is inside a cast: no variable
// of that type, no declaration naming it, and the cast sits in a macro
// argument — the form that leaves the type reachable only through the written
// cast in the AST.

#define CHECK(expr) ((void)(expr))

long take(long v);

long use() {
  CHECK(take(static_cast<probe_off_t>(-2)));
  return take(static_cast<probe_off_t>(-2));
}
