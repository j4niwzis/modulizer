// Implementation unit whose code is guarded by a library feature-test macro
// (`__cpp_lib_char8_t`) that is only defined transitively via the C++ stdlib
// headers (<string_view> pulls in <version>). When <string_view> is replaced
// by `import std.compat`, the macro must still be defined textually or the
// guarded definition compiles out and the linker loses the symbol.
#include <string_view>
#include <ostream>

#ifdef __cpp_lib_char8_t
void write_u8(const char8_t* s) { (void)s; }
#endif

int feat_use(std::ostream& os) { os << 42; return 0; }
