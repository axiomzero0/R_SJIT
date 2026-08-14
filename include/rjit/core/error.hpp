// rjit/core/error.hpp - Error types used across the VM
#pragma once
#include <stdexcept>
#include <string>

namespace rjit {

// Thrown when an R-level error is signaled (e.g., from `stop("...")`
// or an undefined-variable lookup). The interpreter catches these at
// tryCatch boundaries and converts them to R conditions.
class RJitError : public std::runtime_error {
public:
    explicit RJitError(std::string const& msg) : std::runtime_error(msg) {}
};

}  // namespace rjit
