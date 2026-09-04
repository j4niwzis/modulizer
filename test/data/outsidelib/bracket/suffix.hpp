// The other half: it closes what the prefix opened and takes back the guard,
// so the pair can be used again around the next thing.
#ifdef OUTSIDELIB_BRACKET_PREFIX_HPP
#undef OUTSIDELIB_BRACKET_PREFIX_HPP
#else
#error suffix included without prefix
#endif

#pragma pack(pop)
