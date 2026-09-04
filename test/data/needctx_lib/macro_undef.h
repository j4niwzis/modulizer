// The other half: it clears the flag and takes the workaround back, so the
// pair can be used again around the next thing.

#ifndef NEEDCTX_CONFIG_DEF
# error missing or nested #include config_def
#else
# undef NEEDCTX_CONFIG_DEF
#endif

#undef NEEDCTX_WORKAROUND
