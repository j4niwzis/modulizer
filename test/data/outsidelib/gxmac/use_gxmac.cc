#include "outsidelib/api.h"
#include "outsidelib/gxmac/wrapper.hpp"

int main() {
  // An outside declaration the library's own header brought in, which the
  // consumer must include for itself once that header is a module.
  auto g = outsidelib::make_gadget();
  // And one the fragment carries, reached through the header that owns it.
  gxmac::gxmac_facet f;
  return f.id() + (g.v ? 1 : 0);
}
