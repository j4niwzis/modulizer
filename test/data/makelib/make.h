#pragma once

namespace makelib {

// A factory macro: expands to a class AND an inline factory function. `export`
// in front of the invocation only reaches the first declaration, so the macro
// body must carry the export marker on both the class and the factory.
#define MAKE(name, description)                                               \
  class name##Class {                                                         \
   public:                                                                    \
    bool Good() const { return true; }                                        \
  };                                                                          \
  inline name##Class name() { return {}; }

MAKE(Foo, "a foo")

}  // namespace makelib
