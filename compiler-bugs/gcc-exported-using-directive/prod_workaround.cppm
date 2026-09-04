export module m;

namespace foo {

namespace bar {
export struct Thing { int v; };
}

using namespace bar;

// One exported using-DECLARATION per name. This form both compilers carry.
export using bar::Thing;

}  // namespace foo
