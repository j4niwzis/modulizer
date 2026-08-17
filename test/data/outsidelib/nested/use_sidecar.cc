// Quoted and relative: this resolves because the file sits next to this one,
// and it does NOT resolve as an angled include from anywhere else.
#include "sidecar.h"
#include "outsidelib/api.h"

int main() {
  outside::Sidecar s{1};
  return s.n - 1;
}
