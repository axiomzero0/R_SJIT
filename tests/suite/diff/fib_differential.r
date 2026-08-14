#!expect
[1] 610
[1] 610
#!end

# Run fib twice — should get same result regardless of JIT compilation
fib <- function(n) {
    if (n < 2) { n } else { fib(n - 1) + fib(n - 2) }
}
print(fib(15))
print(fib(15))
