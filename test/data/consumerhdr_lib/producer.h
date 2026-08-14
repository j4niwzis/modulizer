#pragma once

namespace consumerhdr_lib {
namespace internal {

// Referenced only from the CONSUMER'S OWN header (helper.h), never from the
// consumer source that includes it — how gmock's headers use gtest's
// internals.
template <typename T>
struct Elem {
  using type = T;
};

// Referenced only from inside this producer header. A use by the library
// itself is not a consumer use and must not export it.
template <typename T>
struct OnlyUsedInternally {
  using type = T;
};

template <typename T>
struct UsesInternally {
  using type = typename OnlyUsedInternally<T>::type;
};

}  // namespace internal
}  // namespace consumerhdr_lib
