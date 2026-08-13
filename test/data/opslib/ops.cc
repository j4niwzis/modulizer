#include <string>

namespace opslib {

bool operator==(const std::string&, const std::string&) { return true; }

bool operator!=(const std::string&, const std::string&) { return false; }

}  // namespace opslib
