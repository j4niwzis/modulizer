#pragma once

namespace macstrip_lib {

#if !defined(PUBLIC_LOG_)

#define PUBLIC_LOG_(severity) ((severity))

inline int log_helper() { return 1; }

#endif  // !defined(PUBLIC_LOG_)

inline int use_log() { return log_helper() + PUBLIC_LOG_(0); }

}  // namespace macstrip_lib
