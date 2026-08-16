#pragma once

namespace usingtarget_lib {
namespace detail {
namespace ordering {

// Not exported on its own: the internal filter keeps `detail` entities out of
// the module's interface.
bool compare_entries(int left, int right);

}  // namespace ordering
}  // namespace detail

// ... but naming it here is what makes it public API, so the target has to be
// exported for this declaration to be exportable at all.
using detail::ordering::compare_entries;

}  // namespace usingtarget_lib
