#pragma once

// Stands in for a library that has already been converted: another run turned
// this into a module and left a macros file beside it.
namespace provider {

struct Thing {
  int value;
};

} // namespace provider

// A macro of the provider's, which a consuming library's own macro bodies
// call. It reaches a consumer through the provider's macros file, or through
// this header where the provider is not a module.
#define PROVIDER_TAG 7
