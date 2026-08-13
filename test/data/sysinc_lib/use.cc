#include "sysinc_lib/api.h"

namespace sysinc_lib {

bool Foo::foo(const char *text) const {
  regex_t re;
  if (regcomp(&re, text, REG_EXTENDED) != 0) return false;
  regfree(&re);
  return true;
}

}  // namespace sysinc_lib
