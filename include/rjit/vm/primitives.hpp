// rjit/vm/primitives.hpp - Built-in primitive functions
//
// These are C++ implementations of R's built-in functions: `c`,
// `length`, `print`, `sum`, etc. They are registered into the
// base environment at Context construction time.

#pragma once
#include "rjit/core/value.hpp"
#include "rjit/core/context.hpp"

namespace rjit {

void register_primitives(Context& ctx);

}  // namespace rjit
