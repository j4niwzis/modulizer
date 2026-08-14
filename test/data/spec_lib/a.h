#pragma once

namespace spec_lib {

// Primary template plus a partial and an explicit specialization: the
// specializations are exported with the primary template and must never carry
// an export marker of their own ([module.interface]/3).
template <typename T>
struct Holder {
  T value;
};

template <typename T>
struct Holder<T*> {
  T* value;
};

template <>
struct Holder<bool> {
  bool value;
};

// A declaration inside an unbraced linkage-specification: the export marker
// belongs in front of the `extern "C"`, not between it and the declaration.
extern "C" int spec_lib_answer();

int plain(int v);

}  // namespace spec_lib
