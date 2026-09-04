// A fragment that DOES guard itself, and still is not a header: it names terms
// its includer defines around it, so read on its own it does not parse. The
// guard says only "read me once", not "read me alone".
// (Boost's detail/utf8_codecvt_facet.hpp is the case this stands for.)
#ifndef GXMAC_BODY_HPP
#define GXMAC_BODY_HPP

GXMAC_OPEN

class GXMAC_DECL gxmac_facet {
public:
  int id() const;
};

GXMAC_CLOSE

#endif  // GXMAC_BODY_HPP
