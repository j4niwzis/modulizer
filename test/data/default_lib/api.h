#pragma once

namespace default_lib {

// A default-value macro guarded by #ifndef so embedders can predefine it.
#ifndef DEFAULT_LIB_DEFAULT_STYLE
#define DEFAULT_LIB_DEFAULT_STYLE "fast"
#endif

const char *style();

}  // namespace default_lib
