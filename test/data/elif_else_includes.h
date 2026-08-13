#pragma once

namespace elif_test {

class BeforeElif {};

#if COND_A
#include <cond_a.h>
#elif COND_B
#include <cond_b.h>
#else
#include <cond_c.h>
#endif

#if PLAIN_IF
#include <plain_if.h>
#else
#include <plain_else.h>
#endif

class AfterIfdef {};

}  // namespace elif_test
