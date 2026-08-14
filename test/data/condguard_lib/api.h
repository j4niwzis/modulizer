#pragma once

// An expression condition (`#if`), not `#ifdef`: the entity below exists only
// where it is 1, so a wrapper re-exporting it unconditionally names something
// that need not exist.
#ifndef CONDGUARD_HAS_FEATURE
#define CONDGUARD_HAS_FEATURE 1
#endif

namespace condguard_lib {

void always_here();

#if CONDGUARD_HAS_FEATURE
void only_with_feature();
#endif

}  // namespace condguard_lib
