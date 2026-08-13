#pragma once

namespace test_lib {
namespace detail {
class Foo {};
}
#define BAR() new ::test_lib::detail::Foo()
}  // namespace test_lib
