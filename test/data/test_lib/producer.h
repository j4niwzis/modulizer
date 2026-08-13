#pragma once

namespace test_lib {
namespace detail {

struct t {};
void foo(t);

template <int N>
struct Size { using Type = int; };

class Foo {
public:
  Foo(int);
  void* foo();
};

typedef int Flag;

template <typename T>
struct Box {};

template <typename T>
using Baz = Box<T>;

template <typename T>
struct Bar { static void Out(const T&); };

template <typename T, typename I>
struct Gen {
  Gen(T, T) {}
};

class FriendTarget;

}  // namespace detail
}  // namespace test_lib

#define BAR(x) ::test_lib::detail::Foo(x).foo()
