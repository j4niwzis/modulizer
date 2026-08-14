#include "sysincl_lib/prod.h"

// A consumer using a type declared in a private per-declaration fragment. The
// fragment is not includable on its own, so the consumer must be pointed at
// the public header that pulls it in.

unsigned long use() { return static_cast<probe_frag_t>(7); }
