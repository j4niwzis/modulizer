#pragma once

namespace test_lib {

#define DECLARE_MACRO(x) namespace detail { class helper {}; } class exported_class : detail::helper { x };

DECLARE_MACRO(public:);

}  // namespace test_lib
