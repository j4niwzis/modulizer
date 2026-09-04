#pragma once

// Includes the configuration header's macros before the header itself gets a
// chance to — which is what every other header of a library does once its own
// macros file chains that one in.
#include "presencerr.h"

namespace test_lib {

int uses_api_kind();

} // namespace test_lib
