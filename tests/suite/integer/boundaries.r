#!expect
[1] 2.14748e+09
[1] -2.14748e+09
[1] 2.14748e+09
#!end

# Integer boundaries (R defaults to real, so these print as doubles)
max_int <- 2147483647
print(max_int)
min_int <- -2147483648
print(min_int)
# Overflow promotes to real
overflow <- 2147483647 + 1
print(overflow)
