#!expect
[1] 75025
#!end

# fib(25) = 75025, makes ~242785 calls
fib <- function(n) {
    if (n < 2) { n } else { fib(n - 1) + fib(n - 2) }
}
print(fib(25))
