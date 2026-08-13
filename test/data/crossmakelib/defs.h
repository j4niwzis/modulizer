#pragma once

namespace crossmakelib {

// A factory macro: expands to a class AND an inline factory function. `export`
// in front of an invocation only reaches the first declaration of the
// expansion, so the export marker must be baked into the macro body (in the
// DEFINING header's macros file), not placed at the invocation in the header
// that uses the macro.
#define MAKE(name, description)                                               \
  class name##Class {                                                         \
   public:                                                                    \
    bool Good() const { return true; }                                        \
  };                                                                          \
  inline name##Class name() { return {}; }

}  // namespace crossmakelib
