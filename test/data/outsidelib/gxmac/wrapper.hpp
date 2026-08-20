#pragma once

// The header that owns that declaration: it defines the terms, reads the
// fragment, and takes them away again. Consumers include this one.
#define GXMAC_OPEN namespace gxmac {
#define GXMAC_CLOSE }
#define GXMAC_DECL

#include "outsidelib/gxmac/body.hpp"

#undef GXMAC_OPEN
#undef GXMAC_CLOSE
#undef GXMAC_DECL
