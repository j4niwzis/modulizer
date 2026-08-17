#pragma once

// A class whose only virtual definition lives out of line, `inline`. The
// definition decides where the vtable and typeinfo are emitted, but only as a
// strong definition — see demote_inline.
namespace test_lib {

struct Cond {
  int value;
};

struct Category {
  virtual ~Category() = default;
  virtual const char *name() const = 0;
  virtual Cond fallback(int ev) const;
};

inline Cond Category::fallback(int ev) const { return Cond{ev}; }

// A non-virtual out-of-line member keeps its `inline`: nothing about vtable
// emission hangs on it, and demoting it would leave the classic build with a
// duplicate symbol in every includer.
struct Plain {
  int twice(int) const;
};

inline int Plain::twice(int v) const { return v * 2; }

// A free function, likewise untouched.
inline int triple(int v) { return v * 3; }

} // namespace test_lib
