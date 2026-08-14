// rjit/core/attributes.cpp
#include "rjit/core/attributes.hpp"
#include "rjit/core/vector.hpp"
#include "rjit/core/gc.hpp"

namespace rjit {

Attributes::Attributes() : HeapObject(TypeTag::kExternalPtr) {}

Value Attributes::get(uint32_t sym_id) const noexcept {
    for (size_t i = 0; i < count_; ++i) {
        if (names_[i] == sym_id) return values_[i];
    }
    return Value::nil();
}

void Attributes::set(uint32_t sym_id, Value v) {
    for (size_t i = 0; i < count_; ++i) {
        if (names_[i] == sym_id) {
            values_[i] = v;
            return;
        }
    }
    if (count_ == names_.size()) {
        names_.resize(names_.empty() ? 4 : names_.size() * 2);
        values_.resize(names_.size());
    }
    names_[count_] = sym_id;
    values_[count_] = v;
    ++count_;
}

bool Attributes::remove(uint32_t sym_id) {
    for (size_t i = 0; i < count_; ++i) {
        if (names_[i] == sym_id) {
            names_[i] = names_[count_-1];
            values_[i] = values_[count_-1];
            --count_;
            return true;
        }
    }
    return false;
}

bool Attributes::next(size_t& cursor, uint32_t& sym_out, Value& val_out) const {
    if (cursor >= count_) return false;
    sym_out = names_[cursor];
    val_out = const_cast<Value&>(values_[cursor]);
    ++cursor;
    return true;
}

void Attributes::trace(Visitor& v) const {
    for (size_t i = 0; i < count_; ++i)
        v.visit(const_cast<Value&>(values_[i]));
}

Value with_attribute(Value v, uint32_t sym_id, Value attr) {
    // Scalars need to be promoted to vectors first.
    if (v.is_scalar()) v = scalar_to_vector(v);
    if (!v.is_vector()) return v;
    // For brevity, attribute storage on the vector is not yet wired up.
    (void)sym_id; (void)attr;
    return v;
}

std::vector<uint32_t> get_class_chain(Value v) {
    return {};
}

Vector* get_names(Value v) {
    return nullptr;
}

}  // namespace rjit
