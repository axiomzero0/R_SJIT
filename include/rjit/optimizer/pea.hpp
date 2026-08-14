// rjit/optimizer/pea.hpp - Partial Escape Analysis
//
// PEA tracks whether allocated objects "escape" their allocation
// site. If they don't, the object is replaced with a VirtualObject
// whose fields are scalar values. If the object escapes later
// (e.g., passed to an unknown function), a Materialize node is
// inserted to construct the real object.
//
// For R, PEA is particularly powerful because:
//   * Short-lived vectors created by `a + b` often don't escape
//     (they're immediately consumed by the next op).
//   * Promise objects often don't escape (they're forced and
//     discarded).
//   * Environment frames often don't escape (the function returns
//     without storing its frame anywhere).
//
// The current implementation is a stub that records allocation
// sites but doesn't yet perform scalar replacement.

#pragma once
#include "rjit/ir/sea_of_nodes.hpp"

namespace rjit {

class Graph;

bool pea(Graph& g);

}  // namespace rjit
