#ifndef OUTSIDELIB_GUARDED_H_INCLUDED
#define OUTSIDELIB_GUARDED_H_INCLUDED

// The ordinary spelling: an include guard around the whole file. It is not a
// condition on the include below it.
#include <outside/gadget.h>

namespace outsidelib {

outside::Gadget guarded_gadget();

} // namespace outsidelib

#endif // OUTSIDELIB_GUARDED_H_INCLUDED
