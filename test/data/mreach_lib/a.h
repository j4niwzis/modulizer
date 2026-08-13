#pragma once

namespace mreach_lib {
namespace internal {
void helper();
}
}

#define MREACH_PUBLIC_MACRO() ::mreach_lib::internal::helper()
