#pragma once

namespace nested_lib {

class Foo {
public:
  class Bar {
  public:
    void connect();
  };
  void run();
};

}  // namespace nested_lib
