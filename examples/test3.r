# Recursive factorial
fact <- function(n) {
    if (n <= 1) {
        1
    } else {
        n * fact(n - 1)
    }
}
print(fact(5))
print(fact(10))

# While loop
i <- 1
s <- 0
while (i <= 10) {
    s <- s + i
    i <- i + 1
}
print(s)

# Fibonacci
fib <- function(n) {
    if (n < 2) {
        n
    } else {
        fib(n - 1) + fib(n - 2)
    }
}
print(fib(10))
print(fib(15))
