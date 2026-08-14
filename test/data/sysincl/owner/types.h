#pragma once

// The public system header that OWNS `probe_off_t`. It lives in a
// subdirectory, so the include name a consumer must write is "owner/types.h" —
// the bare filename "types.h" names nothing.

#include <bits/detail.h>

typedef __probe_off probe_off_t;
