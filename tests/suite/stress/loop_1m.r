#!expect
[1] 5e+11
#!end

# 1M iteration loop
s <- 0
for (i in 1:1000000) {
    s <- s + i
}
print(s)
