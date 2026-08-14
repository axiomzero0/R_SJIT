#!expect
[1] 1
[1] 1
[1] 1
#!end

# IC must invalidate when shape changes (new variable added)
f <- function() { x }
x <- 1
print(f())
y <- 2  # shape change
print(f())
z <- 3  # shape change
print(f())
