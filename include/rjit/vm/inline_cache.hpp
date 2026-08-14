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
#include <unordered_map>
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
    // ICs are keyed by (function, instruction index) because different
    // functions share the same PC space. Using just the PC would cause
    // cache collisions between functions.
    LoadVarIC& load_var_ic(BytecodeFunction* fn, uint32_t instr_idx) {
        auto key = std::make_pair(fn, instr_idx);
        auto& slot = load_var_ics_[key];
        return slot;
    }
    CallIC& call_ic(BytecodeFunction* fn, uint32_t instr_idx) {
        auto key = std::make_pair(fn, instr_idx);
        auto& slot = call_ics_[key];
        return slot;
    }
private:
    using Key = std::pair<BytecodeFunction*, uint32_t>;
    struct KeyHash {
        size_t operator()(Key const& k) const noexcept {
            return reinterpret_cast<uintptr_t>(k.first) ^ (size_t{k.second} << 8);
        }
    };
    std::unordered_map<Key, LoadVarIC, KeyHash> load_var_ics_;
    std::unordered_map<Key, CallIC, KeyHash>    call_ics_;
};

}  // namespace rjit
