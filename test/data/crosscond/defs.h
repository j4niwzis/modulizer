#pragma once

namespace crosscond {

// A factory macro defined inside a conditional block. The export markers for
// the entities it declares must be baked into the correct branch of the macro
// body in the macros file.
#if defined(CROSSCOND_USE_BRANCH)
#define MAKE(name, description)                                               \
  class name##Class {                                                         \
   public:                                                                    \
    bool Good() const { return true; }                                        \
  };                                                                          \
  inline name##Class name() { return {}; }
#else
#define MAKE(name, description)                                               \
  class name##Class {                                                         \
   public:                                                                    \
    int Count() const { return 0; }                                           \
  };                                                                          \
  inline name##Class name() { return {}; }
#endif

}  // namespace crosscond
