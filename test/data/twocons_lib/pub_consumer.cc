#include "twocons_lib/producer.h"

void use_public() {
  twocons_lib::internal::ExposedInternal ei;
  (void)ei;
}
