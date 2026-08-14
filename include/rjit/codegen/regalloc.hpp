// rjit/codegen/regalloc.hpp - Register allocation
//
// We use a linear-scan allocator with a small extension for
// spill-weighted live range splitting. The current implementation
// is a stub; the baseline JIT uses a fixed mapping (everything
// lives in the Value* register file, with operands loaded into
// scratch registers as needed).

#pragma once
#include <cstdint>
#include <vector>
#include "rjit/ir/sea_of_nodes.hpp"

namespace rjit {

struct PhysReg {
    uint8_t id;
    bool    is_float;
};

struct LiveRange {
    Node*   node;
    uint32_t start;
    uint32_t end;
    PhysReg assigned;
};

class RegAlloc {
public:
    RegAlloc();
    void run(Graph& g);
    std::vector<LiveRange> const& ranges() const noexcept { return ranges_; }
private:
    std::vector<LiveRange> ranges_;
};

}  // namespace rjit
