#pragma once

#define LIB_EXPORT_TO_INT(x) \
  namespace lib_test { extern int x; }

LIB_EXPORT_TO_INT(version_num)
LIB_EXPORT_TO_INT(build_id)

namespace lib_test {

class ExportedClass {};

}  // namespace lib_test
