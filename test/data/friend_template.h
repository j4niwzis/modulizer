#pragma once
namespace test_lib {

class Foo {
 public:
  int value = 0;

  template <class T>
  friend class Bar;
};

}  // namespace test_lib
