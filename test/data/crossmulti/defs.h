#pragma once

namespace crossmulti {

// A factory macro invoked from TWO different headers. Both consumers route the
// SAME export markers to the defining header; the macros file must not bake
// duplicates.
#define MAKE(name, description)                                               \
  class name##Class {                                                         \
   public:                                                                    \
    bool Good() const { return true; }                                        \
  };                                                                          \
  inline name##Class name() { return {}; }

}  // namespace crossmulti
