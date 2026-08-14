#!expect
[1] 1
[1] 2
[1] 3
#!end

# Comments are ignored
x <- 1  # inline comment
print(x)
# full line comment
y <- 2
print(y)
z <- 3
print(z)
