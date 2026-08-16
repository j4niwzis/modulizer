#pragma once

namespace nestmember_lib {

class Outer {
 public:
  class Inner {
   public:
    int compute(int value) const;
  };

  int plain(int value) const;
};

inline int Outer::Inner::compute(int value) const { return value + 1; }

inline int Outer::plain(int value) const { return value + 2; }

}  // namespace nestmember_lib
