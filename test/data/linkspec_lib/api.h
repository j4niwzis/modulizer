#pragma once

namespace linkspec_lib {

// Entities this library declares but does not define, each written with an
// `extern` storage-class specifier of its own. Wrapping such a declaration in
// `extern "C++"` would put the specifier inside the linkage-specification.
template <class T>
extern T Make(int n);

extern bool Check(int n);

extern int counter;

}  // namespace linkspec_lib
