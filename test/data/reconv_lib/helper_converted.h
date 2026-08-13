// A shared test helper an earlier --consumers pass already converted. It is
// not parsed as a main file: other consumers #include it, so a pass that only
// reconstructs the file it was handed still trips over this one.
#pragma once
import reconv_lib.producer;

inline int use_internal_helper() {
  reconv_lib::internal::Helper h;
  h.h = 1;
  return h.h;
}
