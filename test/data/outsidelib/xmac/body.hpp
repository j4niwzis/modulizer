// A fragment, not a header. No include guard of its own, and it names terms
// its includer defines around it — read on its own it is not even parseable.
// (Boost.Bind's bind_cc.hpp is the case this stands for; it says so in a
// banner: "Do not include this header directly".)

template <class R>
XMAC_ST R xmac_call(XMAC_CC R (*f)());
