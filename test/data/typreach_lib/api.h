#pragma once

namespace typreach_lib {
namespace detail {

struct Box {
  void use();
};

struct Foo {
  Box bar();
};

Foo baz();

}  // namespace detail

#define RUN() typreach_lib::detail::baz()

}  // namespace typreach_lib
