#!expect
[1] 120
[1] 3.6288e+06
[1] 55
[1] 610
[1] 120
[1] 3.6288e+06
#!end

# Recursion regression: multiple recursive calls must work
fact <- function(n) {
    if (n <= 1) { 1 } else { n * fact(n - 1) }
}
print(fact(5))
print(fact(10))

fib <- function(n) {
    if (n < 2) { n } else { fib(n - 1) + fib(n - 2) }
}
print(fib(10))
print(fib(15))

# Call again — this was the IC collision bug
print(fact(5))
print(fact(10))
