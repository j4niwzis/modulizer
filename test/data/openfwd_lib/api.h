#pragma once

namespace openfwd_lib {

// Declared here and defined by nothing in this library: a consumer (a test)
// defines it so the friendship below grants it access to Widget's internals.
class Accessor;

class Widget {
 public:
  int value() const { return value_; }

 private:
  friend class Accessor;
  int value_ = 7;
};

// Defined right here, so it is a normal module entity.
class Plain {
 public:
  int get() const { return 1; }
};

}  // namespace openfwd_lib
