// One half of a bracket: it opens state that its partner closes, and says so
// itself if it is ever read twice without the partner in between.
// (Boost's config/abi_prefix.hpp is the case this stands for.)
#ifndef OUTSIDELIB_BRACKET_PREFIX_HPP
#define OUTSIDELIB_BRACKET_PREFIX_HPP
#else
#error double inclusion of header outsidelib/bracket/prefix.hpp is an error
#endif

#pragma pack(push, 1)
