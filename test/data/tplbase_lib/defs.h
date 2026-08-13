#pragma once

#include <tuple>
#include <type_traits>
#include <utility>

namespace tplbase_lib {
namespace internal {
template <typename F, typename Tuple, std::size_t... Idx>
auto Bar(F&& f, Tuple&& args, std::index_sequence<Idx...>)
    -> decltype(f(std::get<Idx>(std::forward<Tuple>(args))...)) {
  return f(std::get<Idx>(std::forward<Tuple>(args))...);
}

template <typename F, typename Tuple>
auto Foo(F&& f, Tuple&& args) -> decltype(Bar(
    std::forward<F>(f), std::forward<Tuple>(args),
    std::make_index_sequence<std::tuple_size_v<std::decay_t<Tuple>>>{})) {
  return Bar(std::forward<F>(f), std::forward<Tuple>(args),
             std::make_index_sequence<
                 std::tuple_size_v<std::decay_t<Tuple>>>{});
}

template <typename P>
struct Baz
    // NOLINTNEXTLINE
    : std::integral_constant<bool, bool(!P::value)> {};

template <class T>
class [[nodiscard]] Box {
 public:
  typedef T type;
  typedef const type& const_reference;
};
}
}
