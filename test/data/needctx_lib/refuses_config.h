#ifndef NEEDCTX_REFUSES_CONFIG_H
#define NEEDCTX_REFUSES_CONFIG_H

// An ordinary header that refuses a configuration it cannot work under. The
// macros it tests are somebody else's; it sets nothing and clears nothing, so
// it is not half of anything.
// (gtest-port.h refuses C++ before 17 and a libc++ without wide characters.)

#if !defined(NEEDCTX_LANG) || NEEDCTX_LANG < 201703L
#error versions before C++17 are not supported.
#endif

#ifdef NEEDCTX_NO_WIDE
#error cannot be used without wide character support.
#endif

namespace needctx_refuses {

struct config {
  int v;
};

}  // namespace needctx_refuses

#endif  // NEEDCTX_REFUSES_CONFIG_H
