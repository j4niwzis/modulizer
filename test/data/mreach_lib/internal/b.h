#pragma once

namespace mreach_lib {
namespace internal {
void helper_impl();
}
}

#define MREACH_HELPER_MACRO() ::mreach_lib::internal::helper_impl()
