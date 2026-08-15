#pragma once

namespace injextern_lib {
namespace internal {

// Declared but never defined, each with an `extern` storage-class specifier of
// its own. A module that uses them gets a declaration injected, copied from
// this text and wrapped in `extern "C++"`.
template <class T>
extern T Make(const void*);

extern bool Check(int n);

}  // namespace internal
}  // namespace injextern_lib
