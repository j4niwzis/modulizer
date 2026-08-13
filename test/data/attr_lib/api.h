#pragma once

// A visibility attribute macro (`[[gnu::visibility]]`).
#define ATTR_LIB_API_ [[gnu::visibility("default")]]

namespace attr_lib {

ATTR_LIB_API_ bool foo(bool condition);

}  // namespace attr_lib
