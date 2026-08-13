#include "third_party.h"

void foo() {
  std::variant<int, char> obj = 42;
  (void)obj;
}
