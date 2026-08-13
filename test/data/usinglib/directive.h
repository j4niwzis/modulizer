#pragma once

namespace usinglib {

// The ADL-suppression pattern: entities live in a nested namespace and are
// pulled into the enclosing namespace with a using-directive, so consumers can
// name them without qualification.
namespace adl_guard {

int pointer_thing() { return 42; }

}  // namespace adl_guard

using namespace adl_guard;

}  // namespace usinglib
