#ifndef GUARDSTYLE_HPP_INCLUDED_
#define GUARDSTYLE_HPP_INCLUDED_

// Boost.Iterator spells its guards with a trailing underscore
// (BOOST_ITERATOR_DETAIL_EVAL_IF_DEFAULT_HPP_INCLUDED_). A guard is not a
// macro the library exports, whichever way it is spelled.

namespace guardstyle {

struct thing {
  int v;
};

}  // namespace guardstyle

#endif  // GUARDSTYLE_HPP_INCLUDED_
