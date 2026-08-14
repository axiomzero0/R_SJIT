// rjit/vm/primitives.cpp
#include "rjit/vm/primitives.hpp"
#include "rjit/vm/interpreter.hpp"
#include "rjit/core/closure.hpp"
#include "rjit/core/promise.hpp"
#include "rjit/core/environment.hpp"
#include "rjit/core/vector.hpp"
#include "rjit/core/context.hpp"
#include <cstdio>
#include <cmath>
#include <cstring>

namespace rjit {

namespace prim {

Value print_impl(Context& ctx, Value* args, uint32_t nargs) {
    for (uint32_t i = 0; i < nargs; ++i) {
        Value v = force_if_promise(args[i]);
        if (v.is_real()) std::printf("[1] %g\n", v.as_real());
        else if (v.is_integer()) std::printf("[1] %d\n", v.as_integer());
        else if (v.is_logical()) std::printf("[1] %s\n", v.as_logical() == 1 ? "TRUE" : (v.as_logical() == 0 ? "FALSE" : "NA"));
        else if (v.is_nil()) std::printf("NULL\n");
        else if (v.is_string()) {
            // R-style string printing with escape sequences
            std::string s(ctx.symbol_name(v.as_string()));
            std::printf("[1] \"");
            for (char c : s) {
                switch (c) {
                    case '\\': std::printf("\\\\"); break;
                    case '"':  std::printf("\\\""); break;
                    case '\n': std::printf("\\n"); break;
                    case '\t': std::printf("\\t"); break;
                    case '\r': std::printf("\\r"); break;
                    default: std::printf("%c", c);
                }
            }
            std::printf("\"\n");
        } else if (v.is_vector()) {
            Vector* vec = v.as_vector();
            if (vec->vtype() == VectorType::kReal) {
                for (size_t j = 0; j < vec->length(); ++j)
                    std::printf("[%zu] %g\n", j+1, vec->real_at(j));
            } else if (vec->vtype() == VectorType::kInteger) {
                for (size_t j = 0; j < vec->length(); ++j)
                    std::printf("[%zu] %d\n", j+1, vec->integer_at(j));
            }
        }
    }
    std::fflush(stdout);
    return Value::nil();
}

Value cat_impl(Context& ctx, Value* args, uint32_t nargs) {
    for (uint32_t i = 0; i < nargs; ++i) {
        Value v = force_if_promise(args[i]);
        if (v.is_real()) std::printf("%g ", v.as_real());
        else if (v.is_integer()) std::printf("%d ", v.as_integer());
        else if (v.is_logical()) std::printf("%s ", v.as_logical() == 1 ? "TRUE" : (v.as_logical() == 0 ? "FALSE" : "NA"));
        else if (v.is_string()) std::printf("%s ", std::string(ctx.symbol_name(v.as_string())).c_str());
        else if (v.is_nil()) ; // skip
        else if (v.is_vector()) {
            Vector* vec = v.as_vector();
            for (size_t j = 0; j < vec->length(); ++j) {
                switch (vec->vtype()) {
                    case VectorType::kReal: std::printf("%g ", vec->real_at(j)); break;
                    case VectorType::kInteger: std::printf("%d ", vec->integer_at(j)); break;
                    default: break;
                }
            }
        }
    }
    std::printf("\n");
    return Value::nil();
}

Value length_impl(Context& ctx, Value* args, uint32_t nargs) {
    if (nargs < 1) return Value::integer(0);
    Value v = force_if_promise(args[0]);
    if (v.is_vector()) return Value::integer((int32_t)v.as_vector()->length());
    if (v.is_nil())    return Value::integer(0);
    return Value::integer(1);
}

Value sum_impl(Context& ctx, Value* args, uint32_t nargs) {
    double total = 0;
    bool any_real = false;
    bool any_na = false;
    for (uint32_t i = 0; i < nargs; ++i) {
        Value v = force_if_promise(args[i]);
        if (v.is_real()) { total += v.as_real(); any_real = true; }
        else if (v.is_integer()) {
            int32_t x = v.as_integer();
            if (x == kNaInt) any_na = true;
            else total += x;
        }
        else if (v.is_logical()) {
            int32_t x = v.as_logical();
            if (x == kNaLogical) any_na = true;
            else total += x;
        }
        else if (v.is_vector()) {
            Vector* vec = v.as_vector();
            for (size_t j = 0; j < vec->length(); ++j) {
                switch (vec->vtype()) {
                    case VectorType::kReal: {
                        double d = vec->real_at(j);
                        if (d != d) any_na = true;
                        else total += d;
                        any_real = true;
                        break;
                    }
                    case VectorType::kInteger: {
                        int32_t x = vec->integer_at(j);
                        if (x == kNaInt) any_na = true;
                        else total += x;
                        break;
                    }
                    case VectorType::kLogical: {
                        int32_t x = vec->logical_at(j);
                        if (x == kNaLogical) any_na = true;
                        else total += x;
                        break;
                    }
                    default: break;
                }
            }
        }
    }
    if (any_na) return Value::real(kNaReal);
    if (any_real) return Value::real(total);
    // Pure integer sum (no NA): return integer if fits.
    if (total >= INT32_MIN && total <= INT32_MAX) return Value::integer((int32_t)total);
    return Value::real(total);
}

Value c_impl(Context& ctx, Value* args, uint32_t nargs) {
    if (nargs == 0) return make_real_vector(nullptr, 0);
    // Determine common type
    VectorType common = VectorType::kReal;
    for (uint32_t i = 0; i < nargs; ++i) {
        Value v = force_if_promise(args[i]);
        if (v.is_integer() || v.is_logical()) {
            if (common == VectorType::kReal) common = VectorType::kInteger;
        } else if (v.is_string()) {
            common = VectorType::kString;
            break;
        } else if (v.is_vector()) {
            Vector* vec = v.as_vector();
            if (vec->vtype() == VectorType::kString) {
                common = VectorType::kString;
                break;
            } else if (vec->vtype() == VectorType::kReal) {
                common = VectorType::kReal;
            }
        }
    }
    // Compute total length
    size_t total = 0;
    for (uint32_t i = 0; i < nargs; ++i) {
        Value v = force_if_promise(args[i]);
        if (v.is_vector()) total += v.as_vector()->length();
        else if (!v.is_nil()) total += 1;
    }
    Vector* out = new Vector(common, total);
    size_t pos = 0;
    for (uint32_t i = 0; i < nargs; ++i) {
        Value v = force_if_promise(args[i]);
        if (v.is_nil()) continue;
        if (v.is_vector()) {
            Vector* vec = v.as_vector();
            for (size_t j = 0; j < vec->length(); ++j) {
                switch (common) {
                    case VectorType::kReal: {
                        double d = (vec->vtype() == VectorType::kReal) ? vec->real_at(j) :
                                   (vec->vtype() == VectorType::kInteger) ? (vec->integer_at(j) == kNaInt ? kNaReal : (double)vec->integer_at(j)) :
                                   (vec->vtype() == VectorType::kLogical) ? (vec->logical_at(j) == kNaLogical ? kNaReal : (double)vec->logical_at(j)) :
                                   0.0;
                        out->set_real(pos++, d);
                        break;
                    }
                    case VectorType::kInteger: {
                        int32_t x = (vec->vtype() == VectorType::kInteger) ? vec->integer_at(j) :
                                    (vec->vtype() == VectorType::kLogical) ? vec->logical_at(j) :
                                    0;
                        out->set_integer(pos++, x);
                        break;
                    }
                    default: break;
                }
            }
        } else {
            switch (common) {
                case VectorType::kReal:    out->set_real(pos++, v.is_real() ? v.as_real() : (v.is_integer() ? (v.as_integer() == kNaInt ? kNaReal : (double)v.as_integer()) : (double)v.as_logical())); break;
                case VectorType::kInteger: out->set_integer(pos++, v.is_integer() ? v.as_integer() : v.as_logical()); break;
                default: break;
            }
        }
    }
    return Value::from_heap(TypeTag::kVector, out);
}

Value seq_impl(Context& ctx, Value* args, uint32_t nargs) {
    // seq(from, to, by=1) — simplified
    if (nargs < 2) return Value::nil();
    int32_t from = args[0].is_integer() ? args[0].as_integer() : (int32_t)args[0].as_real();
    int32_t to   = args[1].is_integer() ? args[1].as_integer() : (int32_t)args[1].as_real();
    Vector* v = Vector::range(from, to);
    return Value::from_heap(TypeTag::kVector, v);
}

Value seq_len_impl(Context& ctx, Value* args, uint32_t nargs) {
    if (nargs < 1) return Value::nil();
    int32_t n = args[0].is_integer() ? args[0].as_integer() : (int32_t)args[0].as_real();
    Vector* v = Vector::range(1, n);
    return Value::from_heap(TypeTag::kVector, v);
}

Value seq_along_impl(Context& ctx, Value* args, uint32_t nargs) {
    if (nargs < 1) return Value::nil();
    Value v = force_if_promise(args[0]);
    int32_t n = 0;
    if (v.is_vector()) n = (int32_t)v.as_vector()->length();
    else if (!v.is_nil()) n = 1;
    Vector* out = Vector::range(1, n);
    return Value::from_heap(TypeTag::kVector, out);
}

Value mean_impl(Context& ctx, Value* args, uint32_t nargs) {
    if (nargs < 1) return Value::real(kNaReal);
    Value v = force_if_promise(args[0]);
    if (!v.is_vector()) {
        if (v.is_real()) return v;
        if (v.is_integer()) return Value::real((double)v.as_integer());
        return Value::real(kNaReal);
    }
    Vector* vec = v.as_vector();
    double total = 0;
    size_t n = 0;
    for (size_t i = 0; i < vec->length(); ++i) {
        double d = (vec->vtype() == VectorType::kReal) ? vec->real_at(i) :
                   (vec->vtype() == VectorType::kInteger) ? (vec->integer_at(i) == kNaInt ? kNaReal : (double)vec->integer_at(i)) :
                   (vec->vtype() == VectorType::kLogical) ? (vec->logical_at(i) == kNaLogical ? kNaReal : (double)vec->logical_at(i)) :
                   0.0;
        if (d == d) { total += d; ++n; }
    }
    if (n == 0) return Value::real(kNaReal);
    return Value::real(total / n);
}

Value min_impl(Context& ctx, Value* args, uint32_t nargs) {
    double best = __builtin_inf();
    for (uint32_t i = 0; i < nargs; ++i) {
        Value v = force_if_promise(args[i]);
        if (v.is_vector()) {
            Vector* vec = v.as_vector();
            for (size_t j = 0; j < vec->length(); ++j) {
                double d = (vec->vtype() == VectorType::kReal) ? vec->real_at(j) :
                           (vec->vtype() == VectorType::kInteger) ? (double)vec->integer_at(j) :
                           (double)vec->logical_at(j);
                if (d < best) best = d;
            }
        } else if (v.is_real()) { if (v.as_real() < best) best = v.as_real(); }
        else if (v.is_integer()) { if ((double)v.as_integer() < best) best = v.as_integer(); }
    }
    return Value::real(best);
}

Value max_impl(Context& ctx, Value* args, uint32_t nargs) {
    double best = -__builtin_inf();
    for (uint32_t i = 0; i < nargs; ++i) {
        Value v = force_if_promise(args[i]);
        if (v.is_vector()) {
            Vector* vec = v.as_vector();
            for (size_t j = 0; j < vec->length(); ++j) {
                double d = (vec->vtype() == VectorType::kReal) ? vec->real_at(j) :
                           (vec->vtype() == VectorType::kInteger) ? (double)vec->integer_at(j) :
                           (double)vec->logical_at(j);
                if (d > best) best = d;
            }
        } else if (v.is_real()) { if (v.as_real() > best) best = v.as_real(); }
        else if (v.is_integer()) { if ((double)v.as_integer() > best) best = v.as_integer(); }
    }
    return Value::real(best);
}

Value sapply_impl(Context& ctx, Value* args, uint32_t nargs) {
    // sapply(x, f) — apply f to each element of x, return as vector.
    // For simplicity, only handle integer/real x.
    if (nargs < 2) return Value::nil();
    Value x = force_if_promise(args[0]);
    Value f = force_if_promise(args[1]);
    if (!x.is_vector() || !f.is_closure()) return Value::nil();
    Vector* xv = x.as_vector();
    Vector* out = new Vector(VectorType::kReal, xv->length());
    for (size_t i = 0; i < xv->length(); ++i) {
        Value element;
        switch (xv->vtype()) {
            case VectorType::kReal: element = Value::real(xv->real_at(i)); break;
            case VectorType::kInteger: element = Value::integer(xv->integer_at(i)); break;
            case VectorType::kLogical: element = Value::logical(xv->logical_at(i)); break;
            default: element = Value::nil(); break;
        }
        Value r = ctx.interpreter().call_function(f, &element, 1, ctx.global_env());
        if (r.is_real()) out->set_real(i, r.as_real());
        else if (r.is_integer()) out->set_real(i, (double)r.as_integer());
        else out->set_real(i, kNaReal);
    }
    return Value::from_heap(TypeTag::kVector, out);
}

Value is_na_impl(Context& ctx, Value* args, uint32_t nargs) {
    if (nargs < 1) return Value::logical(0);
    Value v = force_if_promise(args[0]);
    return Value::logical(is_na(v) ? 1 : 0);
}

Value as_real_impl(Context& ctx, Value* args, uint32_t nargs) {
    if (nargs < 1) return Value::real(kNaReal);
    Value v = force_if_promise(args[0]);
    if (v.is_real()) return v;
    if (v.is_integer()) return Value::real(v.as_integer() == kNaInt ? kNaReal : (double)v.as_integer());
    if (v.is_logical()) return Value::real(v.as_logical() == kNaLogical ? kNaReal : (double)v.as_logical());
    return Value::real(kNaReal);
}

Value as_integer_impl(Context& ctx, Value* args, uint32_t nargs) {
    if (nargs < 1) return Value::integer(kNaInt);
    Value v = force_if_promise(args[0]);
    if (v.is_integer()) return v;
    if (v.is_real()) return Value::integer((v.as_real() != v.as_real() || v.as_real() < INT32_MIN || v.as_real() > INT32_MAX) ? kNaInt : (int32_t)v.as_real());
    if (v.is_logical()) return Value::integer(v.as_logical() == kNaLogical ? kNaInt : v.as_logical());
    return Value::integer(kNaInt);
}

}  // namespace prim

void register_primitives(Context& ctx) {
    auto reg = [&](const char* name, Builtin::Fn fn, bool is_special = false) {
        Builtin* b = new Builtin(name, fn, is_special);
        uint32_t sym = ctx.intern_symbol(name);
        ctx.base_env()->define(sym, Value::from_heap(
            is_special ? TypeTag::kSpecial : TypeTag::kBuiltin, b));
    };
    reg("print",     prim::print_impl);
    reg("cat",       prim::cat_impl);
    reg("length",    prim::length_impl);
    reg("sum",       prim::sum_impl);
    reg("c",         prim::c_impl);
    reg("seq",       prim::seq_impl);
    reg("seq_len",   prim::seq_len_impl);
    reg("seq_along", prim::seq_along_impl);
    reg("mean",      prim::mean_impl);
    reg("min",       prim::min_impl);
    reg("max",       prim::max_impl);
    reg("sapply",    prim::sapply_impl);
    reg("is.na",     prim::is_na_impl);
    reg("as.numeric",prim::as_real_impl);
    reg("as.integer",prim::as_integer_impl);
}

}  // namespace rjit
