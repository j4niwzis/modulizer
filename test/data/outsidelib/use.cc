// Includes only the library header, and gets the outside declarations through
// it — which is how nearly every consumer is written.
#include "outsidelib/api.h"

int main() {
  outside::Gadget g = outsidelib::make_gadget();
  return g.v + OUTSIDE_TAG;
}
