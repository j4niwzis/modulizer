#include "chain_lib/decl.h"

namespace chain_lib {

class Foo {
public:
  virtual ~Foo() {}
  virtual int run() const { return 1; }
};

class Bar : public Foo {
public:
  int run() const override { return 2; }
};

int run_bar() { return Bar().run(); }

}  // namespace chain_lib
