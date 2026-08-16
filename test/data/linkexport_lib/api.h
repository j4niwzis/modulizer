#pragma once

namespace linkexport {

// Internal linkage: a namespace-scope `const` is not `extern`, so every
// translation unit gets its own, and no other unit can name this one.
const char kMarker = 7;
constexpr int kLimit = 32;

// External linkage, so these stay nameable from elsewhere.
inline constexpr int kShared = 5;
extern const int kDeclared;

// An unnamed enum has no name for its enumerators to take linkage from.
enum { kLowBits = 4, kWidth = 1 << kLowBits };

// A named one does.
enum Mode { kFast, kSlow };

// Internal linkage and no value to copy: nothing can carry this across.
static int helper() { return 1; }
inline int shared_helper() { return 2; }

struct Handle {};

} // namespace linkexport
