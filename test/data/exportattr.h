#pragma once

#define API_MARK [[gnu::visibility("default")]]

namespace exportattr {

class API_MARK Foo {
 public:
  int v() const { return 1; }
};

}  // namespace exportattr
