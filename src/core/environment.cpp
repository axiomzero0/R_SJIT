// rjit/core/environment.cpp
#include "rjit/core/environment.hpp"
#include "rjit/core/gc.hpp"
#include "rjit/core/context.hpp"
#include "rjit/core/promise.hpp"
#include <algorithm>

namespace rjit {

// ----------------------------- Shape -----------------------------------

Shape* Shape::add(uint32_t sym_id) {
    if (slots_.count(sym_id)) return this;
    Shape* s = new Shape();
    s->slots_ = slots_;
    s->slots_[sym_id] = static_cast<uint32_t>(s->slots_.size());
    s->deopt_ = deopt_;
    return s;
}

Shape* Shape::remove(uint32_t sym_id) {
    Shape* s = new Shape();
    s->slots_ = slots_;
    s->slots_.erase(sym_id);
    s->deopt_ = true;
    return s;
}

// -------------------------- ShapeTable ---------------------------------

ShapeTable& ShapeTable::instance() {
    static ShapeTable t;
    return t;
}

ShapeTable::ShapeTable() {
    empty_ = new Shape();
    empty_->set_id(0);
    shapes_.push_back(empty_);
}

Shape* ShapeTable::transition_add(Shape* current, uint32_t sym_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    uint64_t key = (static_cast<uint64_t>(current->id()) << 32) | sym_id;
    auto it = add_cache_.find(key);
    if (it != add_cache_.end()) return it->second;

    if (current->slot_of(sym_id) != UINT32_MAX) return current;

    Shape* s = new Shape();
    s->slots_ = current->slots_;
    s->slots_[sym_id] = static_cast<uint32_t>(s->slots_.size());
    s->deopt_ = current->is_deopt();
    s->set_id(static_cast<uint32_t>(shapes_.size()));
    shapes_.push_back(s);
    add_cache_[key] = s;
    return s;
}

Shape* ShapeTable::transition_remove(Shape* current, uint32_t sym_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (current->slot_of(sym_id) == UINT32_MAX) return current;
    Shape* s = current->remove(sym_id);
    s->set_id(static_cast<uint32_t>(shapes_.size()));
    shapes_.push_back(s);
    return s;
}

// -------------------------- Environment --------------------------------

Environment::Environment(Environment* parent)
    : HeapObject(TypeTag::kEnvironment), parent_(parent),
      shape_(ShapeTable::instance().empty()) {}

bool Environment::lookup(uint32_t sym_id, Value* out,
                         Environment** cache_env, uint32_t* cache_slot) {
    Environment* e = this;
    while (e) {
        uint32_t slot = e->shape_->slot_of(sym_id);
        if (slot != UINT32_MAX) {
            Binding& b = e->bindings_[slot];
            if (b.state == BindingState::kValue) {
                if (out)         *out = b.value;
                if (cache_env)   *cache_env = e;
                if (cache_slot)  *cache_slot = slot;
                return true;
            }
            if (b.state == BindingState::kPromise) {
                Value v = force_if_promise(b.value);
                b.value = v;
                b.state = BindingState::kValue;
                if (out)         *out = v;
                if (cache_env)   *cache_env = e;
                if (cache_slot)  *cache_slot = slot;
                return true;
            }
            if (b.state == BindingState::kMissing) {
                if (out) *out = Value::missing();
                return true;
            }
        }
        e = e->parent_;
    }
    return false;
}

void Environment::grow_to(uint32_t slot) {
    if (bindings_.size() > slot) return;
    bindings_.resize(slot + 1);
}

void Environment::define(uint32_t sym_id, Value v) {
    uint32_t slot = shape_->slot_of(sym_id);
    if (slot == UINT32_MAX) {
        shape_ = ShapeTable::instance().transition_add(shape_, sym_id);
        slot = shape_->slot_of(sym_id);
        grow_to(slot);
    }
    bindings_[slot].value = v;
    bindings_[slot].state = BindingState::kValue;
    bindings_[slot].flags = 0;
}

bool Environment::set_existing(uint32_t sym_id, Value v) {
    Environment* e = this;
    while (e) {
        uint32_t slot = e->shape_->slot_of(sym_id);
        if (slot != UINT32_MAX) {
            e->bindings_[slot].value = v;
            e->bindings_[slot].state = BindingState::kValue;
            return true;
        }
        e = e->parent_;
    }
    return false;
}

void Environment::remove(uint32_t sym_id) {
    uint32_t slot = shape_->slot_of(sym_id);
    if (slot == UINT32_MAX) return;
    shape_ = ShapeTable::instance().transition_remove(shape_, sym_id);
    bindings_[slot].state = BindingState::kEmpty;
    bindings_[slot].value = Value::nil();
}

void Environment::trace(Visitor& v) const {
    HeapObject* p = const_cast<HeapObject*>(static_cast<HeapObject const*>(parent_));
    v.visit_heap(p);
    const_cast<Environment*>(this)->parent_ = static_cast<Environment*>(p);
    for (auto& b : const_cast<std::vector<Binding>&>(bindings_)) v.visit(b.value);
}

// ----------------------- Global environments ----------------------------

static Environment* g_global = nullptr;
static Environment* g_base   = nullptr;
static Environment* g_empty  = nullptr;

Environment* get_global_env() { return g_global; }
Environment* get_base_env()   { return g_base; }
Environment* get_empty_env()  { return g_empty; }

void reset_environments() {
    delete g_global; g_global = nullptr;
    delete g_base;   g_base   = nullptr;
    delete g_empty;  g_empty  = nullptr;
    g_empty  = new Environment(nullptr);
    g_base   = new Environment(g_empty);
    g_global = new Environment(g_base);
}

}  // namespace rjit
