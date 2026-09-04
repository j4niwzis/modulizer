// An include fragment: no guard of its own, read with the namespace macros
// its includer sets up around it.
FRAGGUARD_BEGIN_NAMESPACE

class facet {
public:
  int id() const;
};

FRAGGUARD_END_NAMESPACE
