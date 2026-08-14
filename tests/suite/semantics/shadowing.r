#!expect
[1] 2
[1] 1
#!end

# Shadowing: f's local x doesn't affect global x
x <- 1
f <- function() { x <- 2; x }
print(f())
print(x)
