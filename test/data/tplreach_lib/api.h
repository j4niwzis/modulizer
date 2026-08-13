#pragma once

namespace tplreach_lib {
namespace internal {
template <typename T>
struct Foo;

template <typename R, typename... Args>
struct Foo<R(Args...)> {
  using Result = R;
};

#define TPLREACH_PUBLIC_MACRO(sig) \
  typename ::tplreach_lib::internal::Foo<sig>::Result
}
}
