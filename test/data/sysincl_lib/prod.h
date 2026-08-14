#pragma once

// A library header that pulls the system umbrella in textually. Consumers get
// the system types through it today; after conversion they must include the
// owning system headers themselves.

#include <umbrella.h>

namespace sysincl_lib {

class Foo {
public:
  void foo() const;
};

}  // namespace sysincl_lib
