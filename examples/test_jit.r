# Test that triggers JIT compilation (100+ calls to fib)
fib <- function(n) {
    if (n < 2) { n } else { fib(n - 1) + fib(n - 2) }
}
# fib(15) makes 1973 calls, well over the 100-call threshold
print(fib(15))
print(fib(15))
