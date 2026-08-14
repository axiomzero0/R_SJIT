# Benchmark: arithmetic loop + recursion
# Same logic will be implemented in Python for comparison.

# 1. Sum 1 to 1,000,000
s <- 0
for (i in 1:1000000) {
    s <- s + i
}
print(s)

# 2. Recursive Fibonacci (single-threaded, depth-limited)
fib <- function(n) {
    if (n < 2) { n } else { fib(n - 1) + fib(n - 2) }
}
print(fib(25))

# 3. Nested loop (matrix-like computation)
total <- 0
for (i in 1:500) {
    for (j in 1:500) {
        total <- total + i * j
    }
}
print(total)
