#include "mid.h"
#include "impl.h"

void use() {
  std::variant<int> v = make();
  (void)v;
}
