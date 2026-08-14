#!expect
[1] 1
[1] 1
[1] 1
#!end

# IC should cache the lookup and return the same value
f <- function() { x }
x <- 1
print(f())
print(f())
print(f())
