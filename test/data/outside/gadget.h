#pragma once

// A header belonging to neither the library being converted nor the standard
// library: another project's, reached through the include path. Nothing
// converts it, so its declarations stay in the global module.
namespace outside {

struct Gadget {
  int v;
};

} // namespace outside

#define OUTSIDE_TAG 7

namespace outside {

// Found only by ADL, and only when a template that uses it is instantiated.
// Nothing in the library's interface names it, so a global module fragment
// pruned to what is decl-reachable does not keep it.
inline bool operator==(Gadget const &a, Gadget const &b) { return a.v == b.v; }

} // namespace outside
