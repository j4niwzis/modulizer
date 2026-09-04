#pragma once

// The header that owns those declarations: it defines the terms, reads the
// fragment, and takes them away again. Consumers include this one.
#define XMAC_CC
#define XMAC_ST

#include "outsidelib/xmac/body.hpp"

#undef XMAC_CC
#undef XMAC_ST
