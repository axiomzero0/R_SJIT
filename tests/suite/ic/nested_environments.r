#!expect
[1] 10
[1] 10
[1] 10
#!end

# Lookup in different environments
# f captures global x; g sets a local x but f still sees global
x <- 10
f <- function() { x }
g <- function() { x <- 20; f() }
print(f())
print(g())
print(f())
