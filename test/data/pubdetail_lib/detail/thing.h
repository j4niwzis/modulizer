#pragma once
namespace pubdetail {
// Public API filed under detail/: the directory organises files here, it does
// not hide names.
struct Thing { int v; };
namespace detail {
struct Hidden { int v; };
}
}
