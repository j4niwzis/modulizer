#pragma once

namespace test_lib {

#ifdef HAS_WINDOWS
class WindowsOnly {
public:
  void win_method();
};
#else
class UnixFallback {};
#endif

#ifndef SKIP_FEATURE
inline int feature_fn() { return 1; }
#endif

#ifndef NO_LEGACY
enum class Legacy { On, Off };
#endif

class AlwaysAvailable {
public:
  void always();
};

}  // namespace test_lib
