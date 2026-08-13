#pragma once

namespace twocons_lib {
namespace internal {
class SecretInternal {
 public:
  int s;
};
class ExposedInternal {
 public:
  int e;
};
}  // namespace internal
class Public {
 public:
  int p;
};
}  // namespace twocons_lib
