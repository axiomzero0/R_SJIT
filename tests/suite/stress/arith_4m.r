#!expect
[1] 500000
#!end

# 4M arithmetic operations
x <- 0.0
for (i in 1:1000000) {
    x <- x + 1.0
    x <- x * 2.0
    x <- x - 1.0
    x <- x / 2.0
}
print(x)
