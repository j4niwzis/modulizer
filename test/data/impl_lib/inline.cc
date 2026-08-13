#include "impl_lib/api.h"

namespace impl_lib {

class Gen {
public:
  explicit Gen(int x) : x_(x) {}
  int get() const { return x_; }

private:
  int x_;
};

}  // namespace impl_lib
