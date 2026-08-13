#include "sysuse_lib/prod.h"

// A consumer that uses the C/POSIX macros and symbols the library header
// pulled in textually, plus the library's fatal macro.

int f() {
  errno = EINTR;
  assert(1);
  pthread_mutex_t m;
  pthread_mutex_lock(&m);
  void *p = NULL;
  ssize_t n = static_cast<ssize_t>(0);
  return p == nullptr ? static_cast<int>(n) : 0;
}

void g() { FATAL(); }
