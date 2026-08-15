#pragma once

namespace blockmode_lib {

// A constant: exporting it is what gives it external linkage in a module
// purview, but that exemption does not apply once the purview is wrapped in
// `extern "C++"`.
constexpr int kLimit = 8;

// Declared here, defined by another module.
class Worker;
void Run(int n);

}  // namespace blockmode_lib
