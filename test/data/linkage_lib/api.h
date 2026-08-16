#pragma once

namespace linkage_lib {

// A const/constexpr variable at namespace scope has internal linkage, so no
// using declaration can name it.
const char kToggle = 127;
constexpr int kLimit = 8;

extern const char kExternalToggle;
int compute(int x);
class Widget {
 public:
  int w;
};

}  // namespace linkage_lib

// A function with C language linkage: it cannot live in the extern "C++" block.
extern "C" {
int linkage_lib_c_entry(int x);
}
