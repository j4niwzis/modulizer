namespace impl_lib {

// A file-local helper that happens to share its name with a cross-module
// entity (e.g. an overload of a function declared in a header). It has
// internal linkage and must never be prefixed with extern "C++".
static int foo(int x) { return x + 1; }

int use_foo() { return foo(5); }

}  // namespace impl_lib
