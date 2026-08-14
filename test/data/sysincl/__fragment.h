#pragma once

// A per-declaration fragment of the kind compilers ship (`__stddef_size_t.h`
// and friends): not includable on its own, only ever pulled in by the public
// header that gathers the fragments. Here the public parent is the umbrella
// itself, so the umbrella is the right answer for this type.

typedef unsigned long probe_frag_t;
