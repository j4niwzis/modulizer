#pragma once

#if defined(PLATFORM_X)
#define CODEBLOCK_HELPER(x) x * 2
namespace codeblock {
class Bar;
}
#else
#define CODEBLOCK_HELPER(x) x
#endif

namespace codeblock {

class Baz {
 public:
  int v() const { return CODEBLOCK_HELPER(3); }
};

}  // namespace codeblock
