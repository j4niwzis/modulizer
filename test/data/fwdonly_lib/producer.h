#pragma once

namespace fwdonly_lib {
namespace internal {

// Forward-declared ONLY in the library: the consumer defines the class itself
// (the consumer-defined-class pattern). Exporting it would make the module
// entity collide with the consumer's own global-module definition.
class Foo;

}  // namespace internal
}  // namespace fwdonly_lib
