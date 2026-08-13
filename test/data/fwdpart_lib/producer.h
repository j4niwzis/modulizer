#pragma once

namespace fwdpart_lib {

template <typename T>
struct Box;

template <typename R, typename... Args>
struct Box<R(Args...)> {
  using Bar = R;
};

template <typename F>
using Foo = typename Box<F>::Bar;

}  // namespace fwdpart_lib
