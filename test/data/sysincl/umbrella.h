#pragma once

// An umbrella system header that reaches the owning header transitively. It
// provides `probe_off_t` only because it happens to include "owner/types.h" —
// another platform's umbrella of the same name need not, which is why a
// consumer must be told the owning header instead of this one.

#include <__fragment.h>
#include <owner/types.h>
