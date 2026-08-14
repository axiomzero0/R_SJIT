// rjit/optimizer/passes.hpp - Sea-of-Nodes optimization passes
//
// All passes implement a uniform interface: `bool run(Graph&)`,
// returning true if the graph was modified. The driver runs passes
// to a fixed point.

#pragma once
#include "rjit/ir/sea_of_nodes.hpp"

namespace rjit {

// Dead code elimination: removes nodes with no uses (except for
// nodes with side effects, which are kept).
bool dce(Graph& g);

// Global value numbering: replaces equivalent nodes with a single
// representative.
bool gvn(Graph& g);

// Sparse conditional constant propagation: uses the type lattice
// to propagate constants and prune unreachable branches.
bool sccp(Graph& g);

// Loop-invariant code motion: hoists loop-invariant computations
// out of loops.
bool licm(Graph& g);

// Common subexpression elimination (a subset of GVN).
bool cse(Graph& g);

// Constant folding.
bool constant_fold(Graph& g);

// Strength reduction (e.g., x*1 -> x).
bool strength_reduce(Graph& g);

// Run all passes to a fixed point. Returns the number of iterations.
uint32_t run_all_passes(Graph& g);

}  // namespace rjit
