export module m;

namespace foo {

// The inner namespace is meant to be reached through the directive below,
// never by naming it.
namespace bar {
export struct Thing { int v; };
}

// [module.interface]/2: a using-directive is a block-declaration, so one at
// namespace scope inside an export-declaration is an exported declaration.
// The example in that subclause marks this form OK.
export using namespace bar;

}  // namespace foo
