// rjit/core/environment.hpp - R environments with shape-based lookup
//
// R environments are mutable key->binding maps organized in a parent
// chain (lexical scoping). They are heavily used: every function call
// pushes a new environment, and variable lookups walk the chain.
//
// To make this fast, we use a hidden-class ("shape") scheme:
//
//   * Every environment has a *shape id* identifying the set of keys
//     it currently contains and the slot index of each.
//   * Adding a new key transitions to a new shape (looked up in a
//     global shape table).
//   * Variable lookup is then: probe inline cache for shape id; if
//     hit, return slot[id]; if miss, fall back to hash lookup.
//
// This mirrors V8's hidden classes but adapted for R's quirks:
//   * R bindings can be *promises* (lazy), so each slot has a state.
//   * R allows `rm(x)` to remove a binding, which forces a shape
//     transition to a "deoptimized" shape that uses hash lookup.
//   * R bindings can be locked (lockBinding), which we record as a
//     per-slot flag rather than a separate shape.
//
// No hardcoded limits: slot indices use uint32_t, the shape table
// grows on demand, and chains have no depth limit.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "rjit/core/value.hpp"
#include "rjit/core/gc.hpp"

namespace rjit {

class Environment;
class Symbol;

// Binding state: a slot can hold a forced value or an unforced promise.
enum class BindingState : uint8_t {
    kEmpty     = 0,  // binding does not exist
    kValue     = 1,  // holds a forced Value
    kPromise   = 2,  // holds a Promise*
    kMissing   = 3,  // missing-argument marker
};

struct Binding {
    BindingState state = BindingState::kEmpty;
    uint8_t      flags = 0;  // bit0: locked, bit1: active binding, bit2: removed
    Value        value;
};

// Shape: a frozen mapping {symbol -> slot index}. Shapes are *immutable*
// once created. Transitions (add/remove) produce new shapes.
class Shape {
public:
    using SlotMap = std::unordered_map<uint32_t, uint32_t>;  // sym_id -> slot

    Shape() = default;

    uint32_t id() const noexcept { return id_; }
    void set_id(uint32_t id) noexcept { id_ = id; }

    size_t size() const noexcept { return slots_.size(); }

    // Look up the slot index for a symbol, or UINT32_MAX if not present.
    uint32_t slot_of(uint32_t sym_id) const noexcept {
        auto it = slots_.find(sym_id);
        if (it == slots_.end()) return UINT32_MAX;
        return it->second;
    }

    // Transition: add a new symbol, returning the resulting shape.
    // If the symbol already exists, returns this.
    Shape* add(uint32_t sym_id);

    // Transition: remove a symbol. Always produces a "deopt" shape
    // (we don't try to find an existing matching shape with that key
    // removed, because the cost of the search usually exceeds the
    // benefit; the deopt shape uses hash lookup).
    Shape* remove(uint32_t sym_id);

    // True if this shape has been "deoptimized" (uses hash lookup).
    bool is_deopt() const noexcept { return deopt_; }
    void mark_deopt() noexcept { deopt_ = true; }

    void trace(Visitor& v) const {}  // shapes contain no GC pointers

    SlotMap const& map() const noexcept { return slots_; }

    // Make these public so that shape transitions can copy them.
    // (A production implementation would use friend declarations
    // or a builder class.)
    uint32_t id_      = 0;
    bool     deopt_   = false;
    SlotMap  slots_;  // sym_id -> slot index
};

// Global shape table. All shapes are interned so pointer equality can
// be used for shape comparisons.
class ShapeTable {
public:
    static ShapeTable& instance();

    // The "empty" shape (no bindings).
    Shape* empty() noexcept { return empty_; }

    // Add a binding to `current` shape, returning the resulting shape
    // (interned). Thread-safe.
    Shape* transition_add(Shape* current, uint32_t sym_id);

    // Remove a binding (always deopt).
    Shape* transition_remove(Shape* current, uint32_t sym_id);

    // Statistics
    size_t shape_count() const noexcept { return shapes_.size(); }

private:
    ShapeTable();
    std::mutex                      mtx_;
    std::vector<Shape*>             shapes_;
    Shape*                          empty_;
    std::unordered_map<uint64_t, Shape*> add_cache_;  // (current.id << 32) | sym_id -> shape
};

class Environment : public HeapObject {
public:
    RJIT_DECLARE_GC_NEW(Environment)

    explicit Environment(Environment* parent = nullptr);

    Environment* parent() const noexcept { return parent_; }

    // Shape access (for inline caches)
    Shape* shape() const noexcept { return shape_; }
    uint32_t shape_id() const noexcept { return shape_->id(); }

    // Lookup. Returns true and sets `out` if found; returns false
    // otherwise. The lookup walks the parent chain. The inline cache
    // hint (env, shape, slot) is filled in `cache_env`/`cache_slot`
    // for the level where the binding was found.
    bool lookup(uint32_t sym_id, Value* out,
                Environment** cache_env = nullptr,
                uint32_t* cache_slot = nullptr);

    // Direct slot access (for JIT-compiled fast paths). Requires the
    // caller to have verified shape.
    Value  slot_get(uint32_t slot) const noexcept { return bindings_[slot].value; }
    void   slot_set(uint32_t slot, Value v) noexcept { bindings_[slot].value = v; bindings_[slot].state = BindingState::kValue; }

    // Define a new variable. Transitions shape.
    void define(uint32_t sym_id, Value v);

    // Set an existing variable (walks parent chain). Returns false if
    // not found (caller should signal an R error).
    bool set_existing(uint32_t sym_id, Value v);

    // Remove a variable. Transitions to a deopt shape.
    void remove(uint32_t sym_id);

    // Number of bindings
    size_t size() const noexcept { return shape_->size(); }

    void trace(Visitor& v) const override;

private:
    friend class EnvironmentMutator;  // for GC tracing
    Environment* parent_;
    Shape*       shape_;
    std::vector<Binding> bindings_;  // indexed by slot

    void grow_to(uint32_t slot);
};

// Global environment (top of the lexical chain for user code).
// These are free functions; the Context class also has methods with
// the same name (Context::global_env() etc.) which return its own
// cached pointers. To avoid ADL ambiguity, the free functions are
// named with a `get_` prefix.
Environment* get_global_env();
Environment* get_base_env();
Environment* get_empty_env();

// Reset globals (for tests)
void reset_environments();

}  // namespace rjit
