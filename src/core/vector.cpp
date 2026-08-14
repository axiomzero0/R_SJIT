// rjit/core/vector.cpp
#include "rjit/core/vector.hpp"
#include "rjit/core/gc.hpp"
#include "rjit/core/context.hpp"
#include "rjit/core/attributes.hpp"
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <new>

namespace rjit {

const char* vector_type_name(VectorType vt) noexcept {
    switch (vt) {
        case VectorType::kLogical:  return "logical";
        case VectorType::kInteger:  return "integer";
        case VectorType::kReal:     return "double";
        case VectorType::kString:   return "character";
        case VectorType::kComplex:  return "complex";
        case VectorType::kRaw:      return "raw";
        case VectorType::kList:     return "list";
    }
    return "<unknown>";
}

size_t element_size(VectorType vt) noexcept {
    switch (vt) {
        case VectorType::kLogical:  return sizeof(int32_t);
        case VectorType::kInteger:  return sizeof(int32_t);
        case VectorType::kReal:     return sizeof(double);
        case VectorType::kString:   return sizeof(uint32_t);
        case VectorType::kComplex:  return 2 * sizeof(double);
        case VectorType::kRaw:      return 1;
        case VectorType::kList:     return sizeof(Value);
    }
    return 0;
}

Vector::Vector(VectorType vt, size_t length)
    : HeapObject(TypeTag::kVector), vtype_(vt), repr_(VectorRepr::kConcrete),
      length_(length), data_(nullptr) {
    allocate_data();
}

Vector::~Vector() {
    free_data();
}

void Vector::allocate_data() {
    if (length_ == 0) {
        data_ = nullptr;
        return;
    }
    size_t sz = element_size(vtype_) * length_;
    // Align to 16 bytes for SIMD.
    size_t aligned = (sz + 15) & ~size_t(15);
    data_ = std::aligned_alloc(16, aligned);
    if (!data_) {
        std::fprintf(stderr, "Vector: out of memory for %zu bytes\n", aligned);
        std::abort();
    }
    std::memset(data_, 0, aligned);
}

void Vector::free_data() {
    if (data_) {
        std::free(data_);
        data_ = nullptr;
    }
}

int32_t Vector::integer_at(size_t i) const noexcept {
    return static_cast<int32_t const*>(data_)[i];
}
double Vector::real_at(size_t i) const noexcept {
    return static_cast<double const*>(data_)[i];
}
int32_t Vector::logical_at(size_t i) const noexcept {
    return static_cast<int32_t const*>(data_)[i];
}
uint32_t Vector::string_at(size_t i) const noexcept {
    return static_cast<uint32_t const*>(data_)[i];
}
Value Vector::list_at(size_t i) const noexcept {
    return static_cast<Value const*>(data_)[i];
}

void Vector::set_integer(size_t i, int32_t v) noexcept {
    static_cast<int32_t*>(data_)[i] = v;
}
void Vector::set_real(size_t i, double v) noexcept {
    static_cast<double*>(data_)[i] = v;
}
void Vector::set_logical(size_t i, int32_t v) noexcept {
    static_cast<int32_t*>(data_)[i] = v;
}
void Vector::set_string(size_t i, uint32_t v) noexcept {
    static_cast<uint32_t*>(data_)[i] = v;
}
void Vector::set_list(size_t i, Value v) noexcept {
    static_cast<Value*>(data_)[i] = v;
}

Vector* Vector::coerce_to(VectorType target) const {
    if (vtype_ == target) return const_cast<Vector*>(this);
    Vector* out = new Vector(target, length_);
    switch (vtype_) {
        case VectorType::kInteger:
            switch (target) {
                case VectorType::kReal:
                    for (size_t i = 0; i < length_; ++i) {
                        int32_t v = integer_at(i);
                        out->set_real(i, v == kNaInt ? kNaReal : static_cast<double>(v));
                    }
                    break;
                case VectorType::kLogical:
                    for (size_t i = 0; i < length_; ++i)
                        out->set_logical(i, integer_at(i) == kNaInt ? kNaLogical : (integer_at(i) != 0));
                    break;
                default: break;
            }
            break;
        case VectorType::kLogical:
            switch (target) {
                case VectorType::kReal:
                    for (size_t i = 0; i < length_; ++i) {
                        int32_t v = logical_at(i);
                        out->set_real(i, v == kNaLogical ? kNaReal : static_cast<double>(v));
                    }
                    break;
                case VectorType::kInteger:
                    for (size_t i = 0; i < length_; ++i)
                        out->set_integer(i, logical_at(i) == kNaLogical ? kNaInt : logical_at(i));
                    break;
                default: break;
            }
            break;
        case VectorType::kReal:
            switch (target) {
                case VectorType::kInteger: {
                    bool any_na = false;
                    for (size_t i = 0; i < length_; ++i) {
                        double d = real_at(i);
                        if (d != d || d < INT32_MIN || d > INT32_MAX) {
                            out->set_integer(i, kNaInt);
                            any_na = true;
                        } else {
                            out->set_integer(i, static_cast<int32_t>(d));
                        }
                    }
                    if (any_na) out->flags_.clear(VectorFlags::kNoNA);
                    break;
                }
                case VectorType::kLogical:
                    for (size_t i = 0; i < length_; ++i) {
                        double d = real_at(i);
                        if (d != d) out->set_logical(i, kNaLogical);
                        else out->set_logical(i, d != 0.0);
                    }
                    break;
                default: break;
            }
            break;
        default:
            // Other coercions are not commonly needed by the JIT fast paths.
            break;
    }
    return out;
}

Vector* Vector::concat(Vector* const* parts, size_t n_parts) {
    if (n_parts == 0) return new Vector(VectorType::kReal, 0);
    VectorType common = parts[0]->vtype_;
    size_t total = 0;
    for (size_t i = 0; i < n_parts; ++i) {
        if (parts[i]->vtype_ != common) {
            // Promote to real if mixed numeric, otherwise list.
            if (parts[i]->vtype_ == VectorType::kReal && common != VectorType::kList)
                common = VectorType::kReal;
            else if (parts[i]->vtype_ == VectorType::kList || common == VectorType::kList)
                common = VectorType::kList;
        }
        total += parts[i]->length_;
    }
    Vector* out = new Vector(common, total);
    size_t pos = 0;
    for (size_t i = 0; i < n_parts; ++i) {
        Vector* p = parts[i];
        if (p->vtype_ != common) {
            Vector* coerced = p->coerce_to(common);
            for (size_t j = 0; j < coerced->length_; ++j) {
                switch (common) {
                    case VectorType::kReal:    out->set_real(pos+j, coerced->real_at(j)); break;
                    case VectorType::kInteger: out->set_integer(pos+j, coerced->integer_at(j)); break;
                    case VectorType::kLogical: out->set_logical(pos+j, coerced->logical_at(j)); break;
                    case VectorType::kString:  out->set_string(pos+j, coerced->string_at(j)); break;
                    case VectorType::kList:    out->set_list(pos+j, coerced->list_at(j)); break;
                    default: break;
                }
            }
        } else {
            size_t es = element_size(common);
            std::memcpy(static_cast<char*>(out->data_) + pos*es, p->data_, p->length_*es);
        }
        pos += p->length_;
    }
    return out;
}

Vector* Vector::range(int64_t start, int64_t end) {
    // We allocate a *deferred* vector but materialize immediately for
    // simplicity in the initial implementation. The optimizer will
    // recognize the range pattern via the `range_start_`/`range_step_`
    // fields (which we set even on concrete vectors, so that the JIT
    // can pattern-match `1:n` even after materialization).
    int64_t step = (end >= start) ? 1 : -1;
    int64_t n = (end >= start) ? (end - start + 1) : (start - end + 1);
    if (n < 0) n = 0;
    Vector* v = new Vector(VectorType::kInteger, static_cast<size_t>(n));
    v->repr_ = VectorRepr::kRange;
    v->range_start_ = start;
    v->range_step_ = step;
    // Materialize
    for (int64_t i = 0; i < n; ++i)
        v->set_integer(static_cast<size_t>(i), static_cast<int32_t>(start + i*step));
    return v;
}

void Vector::materialize() {
    // Already materialized if repr_ is kConcrete.
    if (repr_ == VectorRepr::kConcrete) return;
    // For kRange/kRepeat/kCoercion, the data_ is already filled in
    // by the constructor (we eagerly materialize in this initial
    // implementation). So this is a no-op.
    repr_ = VectorRepr::kConcrete;
}

void Vector::trace(Visitor& v) const {
    if (vtype_ == VectorType::kList && data_) {
        Value* items = static_cast<Value*>(data_);
        for (size_t i = 0; i < length_; ++i) v.visit(items[i]);
    }
    if (altrep_dep_) {
        HeapObject* p = const_cast<HeapObject*>(static_cast<HeapObject const*>(altrep_dep_));
        v.visit_heap(p);
        const_cast<Vector*>(this)->altrep_dep_ = static_cast<Vector*>(p);
    }
}

Value make_real_vector(double const* data, size_t n) {
    Vector* v = new Vector(VectorType::kReal, n);
    for (size_t i = 0; i < n; ++i) v->set_real(i, data[i]);
    return Value::from_heap(TypeTag::kVector, v);
}
Value make_int_vector(int32_t const* data, size_t n) {
    Vector* v = new Vector(VectorType::kInteger, n);
    for (size_t i = 0; i < n; ++i) v->set_integer(i, data[i]);
    return Value::from_heap(TypeTag::kVector, v);
}
Value make_logical_vector(int32_t const* data, size_t n) {
    Vector* v = new Vector(VectorType::kLogical, n);
    for (size_t i = 0; i < n; ++i) v->set_logical(i, data[i]);
    return Value::from_heap(TypeTag::kVector, v);
}
Value make_string_vector(uint32_t const* data, size_t n) {
    Vector* v = new Vector(VectorType::kString, n);
    for (size_t i = 0; i < n; ++i) v->set_string(i, data[i]);
    return Value::from_heap(TypeTag::kVector, v);
}

Value scalar_to_vector(Value scalar) {
    switch (scalar.tag()) {
        case TypeTag::kReal:    { double v = scalar.as_real();    return make_real_vector(&v, 1); }
        case TypeTag::kInteger: { int32_t v = scalar.as_integer(); return make_int_vector(&v, 1); }
        case TypeTag::kLogical: { int32_t v = scalar.as_logical(); return make_logical_vector(&v, 1); }
        case TypeTag::kString:  { uint32_t v = scalar.as_string(); return make_string_vector(&v, 1); }
        case TypeTag::kNil:     return make_real_vector(nullptr, 0);
        default:                return scalar;  // already a heap object
    }
}

}  // namespace rjit
