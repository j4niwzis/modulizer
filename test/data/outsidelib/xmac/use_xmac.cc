#include "outsidelib/api.h"
#include "outsidelib/xmac/wrapper.hpp"

int main() {
  // Reaches a declaration the fragment carries, through the header that owns it.
  return xmac_call<int>(nullptr) ? 0 : 1;
}
