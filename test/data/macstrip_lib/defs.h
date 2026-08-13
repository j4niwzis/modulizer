#pragma once

namespace macstrip_lib {

// Public macro with a doc comment.
//
// This macro is used by consumers of the library.
#define PUBLIC_OP(x) ((x) + 1)

// Private helper: defined then undefined before the end of the header.
#define PRIVATE_HELPER(x) ((x) * 2)

inline int use_private(int value) { return PRIVATE_HELPER(value); }

#undef PRIVATE_HELPER

}  // namespace macstrip_lib
