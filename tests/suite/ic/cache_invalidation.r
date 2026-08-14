#!expect
[1] 1
[1] 3
#!end

# IC must return updated value after variable is reassigned
f <- function() { x }
x <- 1
print(f())
x <- 3
print(f())
