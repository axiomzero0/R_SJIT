// rjit/vm/inline_cache.hpp - Inline caches for variable lookup and calls
//
// ICs record (shape, slot) or (callee, target) tuples observed at
// runtime. The interpreter fast path checks the IC entry first; on
// miss, it falls back to the slow path and (if appropriate) updates
// the entry.
//
// ICs are per bytecode-instruction. The interpreter indexes into
// the IC table by instruction index.

#pragma once
#include <cstdint>
#include <vector>
#include "rjit/core/value.hpp"

namespace rjit {

class Environment;
class Closure;
class Builtin;

// Inline cache for LOAD_VAR. Records the (env, shape_id, slot) where
// the binding was found on a previous execution.
struct LoadVarIC {
    Environment* env       = nullptr;  // environment where found
    uint32_t     shape_id  = 0;        // shape id at that environment
    uint32_t     slot      = UINT32_MAX;
    uint32_t     misses    = 0;        // miss count (for transitioning to megamorphic)

    bool valid() const noexcept { return env != nullptr; }
    void invalidate() noexcept { env = nullptr; slot = UINT32_MAX; misses = 0; }
};

// Inline cache for CALL. Records the callee observed at this call
// site. Monomorphic ICs have one entry; polymorphic ICs have a
// small array (up to 4 by default).
struct CallICEntry {
    Closure* callee = nullptr;
    uint32_t hits   = 0;
};

struct CallIC {
    static constexpr uint32_t kMaxPoly = 4;
    CallICEntry entries[kMaxPoly];
    uint32_t    count = 0;
    bool        megamorphic = false;

    bool valid() const noexcept { return count > 0; }
    void invalidate() noexcept { count = 0; megamorphic = false; }
};

class InlineCacheTable {
public:
    LoadVarIC& load_var_ic(uint32_t instr_idx) {
        if (load_var_ics_.size() <= instr_idx)
            load_var_ics_.resize(instr_idx + 1);
        return load_var_ics_[instr_idx];
    }
    CallIC& call_ic(uint32_t instr_idx) {
        if (call_ics_.size() <= instr_idx)
            call_ics_.resize(instr_idx + 1);
        return call_ics_[instr_idx];
    }
private:
    std::vector<LoadVarIC> load_var_ics_;
    std::vector<CallIC>    call_ics_;
};

}  // namespace rjit
