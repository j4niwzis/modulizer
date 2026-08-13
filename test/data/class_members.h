#pragma once

namespace test_lib {

class Outer {
public:
  typedef int MemberTypedef;
  using MemberAlias = float;
  enum class MemberEnum { A, B };

  class NestedClass {
  public:
    void method();
  };

  static const int kConst = 42;
  void func();
};

typedef int TopLevelTypedef;
using TopLevelAlias = double;
enum TopLevelEnum { X, Y };

CLASS_EXPORT class AlreadyExported {};

}  // namespace test_lib
