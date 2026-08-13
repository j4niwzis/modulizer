#pragma once

#define GUARDED_INC_VERSION 1

namespace guarded_inc {

class Unconditional {};

#ifdef HAS_FEATURE_X
#include <feature_x.h>
#endif

#ifndef SKIP_SUPPORT
#include <support_lib.h>
#endif

// Nested guards
#ifdef PLATFORM_A
#ifdef SUB_FEATURE
#include <sub_feature.h>
#endif
#endif

class AfterGuards {};

}  // namespace guarded_inc
