#pragma once

namespace crossattr {

// A factory macro whose body declares a class with an attribute list after the
// class keyword. The export marker must be inserted before `class`, not inside
// the attribute, wherever the macro expands.
#define MAKE(name, description)                                               \
  class [[nodiscard]] name##Class {                                           \
   public:                                                                    \
    bool Good() const { return true; }                                        \
  };                                                                          \
  inline name##Class name() { return {}; }

}  // namespace crossattr
