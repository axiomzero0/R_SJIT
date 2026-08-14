#!expect
[1] 10
#!end

# Lexical scoping: f captures the global x, not g's local x
x <- 10
f <- function() { x }
g <- function() { x <- 20; f() }
print(g())
