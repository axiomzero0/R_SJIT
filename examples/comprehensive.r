# Comprehensive R-JIT test

# 1. Basic arithmetic
print(2 + 3)
print(10 - 4)
print(6 * 7)
print(20 / 4)

# 2. Variables
x <- 10
y <- 20
print(x + y)

# 3. Vectors
v <- 1:5
print(v)
print(sum(v))
print(length(v))
print(mean(v))

# 4. Closures
add <- function(a, b) { a + b }
print(add(3, 4))

# 5. Recursion
fact <- function(n) {
    if (n <= 1) { 1 } else { n * fact(n - 1) }
}
print(fact(5))
print(fact(10))

# 6. Fibonacci
fib <- function(n) {
    if (n < 2) { n } else { fib(n - 1) + fib(n - 2) }
}
print(fib(10))
print(fib(15))

# 7. For loop
total <- 0
for (i in 1:100) { total <- total + i }
print(total)

# 8. While loop
i <- 1
s <- 0
while (i <= 10) { s <- s + i; i <- i + 1 }
print(s)

# 9. Conditionals
x <- 5
if (x > 3) { print(100) } else { print(200) }

# 10. Nested calls
print(add(add(1, 2), add(3, 4)))

# 11. Multiple recursive calls (the bug that was fixed)
print(fact(5))
print(fact(10))
print(fib(10))
print(fib(10))
