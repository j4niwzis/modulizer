#pragma once

// A library header that wraps its declarations in the bracket, the way a
// library does to fix layout for what it declares.
#include <outside/gadget.h>
#include "outsidelib/bracket/prefix.hpp"

namespace outsidelib {

struct Bracketed {
  outside::Gadget g;
};

}  // namespace outsidelib

#include "outsidelib/bracket/suffix.hpp"
