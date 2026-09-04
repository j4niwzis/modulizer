// One half of a bracket, kept by a macro rather than by pragma state: it sets
// the flag macro_undef.h clears, and says so itself if it is ever read twice
// without that half in between. No include guard: being read many times, once
// around each thing it configures, is the whole point.
// (Boost.Iterator's detail/config_def.hpp is the case this stands for.)

#ifdef NEEDCTX_CONFIG_DEF
# error you have nested config_def #inclusion.
#else
# define NEEDCTX_CONFIG_DEF
#endif

#define NEEDCTX_WORKAROUND 1
