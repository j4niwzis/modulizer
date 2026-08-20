#pragma once

#include <outside/gadget.h>

// Reached only on another platform. Handing this to consumers unconditionally
// is handing them a header that cannot compile.
#if defined(OUTSIDELIB_OTHER_PLATFORM)
#include <outside/winonly.h>
#endif

// The conversion's own guard, which every generated header carries. It says
// nothing about whether an include is conditional in the original.
#ifndef OUTSIDELIB_USE_MODULES
// Under a PROVIDER's guard as well, which is how a converted dependency's
// header is written once the conversion has seen it. Both guards are the
// conversion's own doing and neither says the include was conditional.
#if !defined(OUTSIDE_IMPORT_MODULES)
#include <outside/gadget.h>
#endif
#endif

namespace outsidelib {

// The outside type appears in the interface, so it survives pruning. The
// operator that compares it does not.
struct Holder {
  outside::Gadget g;

  friend bool operator==(Holder const &a, Holder const &b) {
    return a.g == b.g;
  }
};

} // namespace outsidelib
