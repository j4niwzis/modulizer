#pragma once

#ifndef MULTI_COND_MACRO
// The user didn't tell us, so figure it out. The multi-line `#if` condition
// below ends with a backslash continuation; the continuation line must not be
// mistaken for a code line (which would suppress emitting this guard block).
#if defined(_MSC_VER) && \
    defined(_CPPRTTI)
#define MULTI_COND_MACRO 1
#else
#define MULTI_COND_MACRO 0
#endif
#endif  // MULTI_COND_MACRO

#define USE_MULTI_COND() MULTI_COND_MACRO
