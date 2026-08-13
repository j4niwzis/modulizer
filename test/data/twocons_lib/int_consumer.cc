#include "twocons_lib/producer.h"

void use_internal() {
  twocons_lib::internal::SecretInternal si;
  (void)si;
}
