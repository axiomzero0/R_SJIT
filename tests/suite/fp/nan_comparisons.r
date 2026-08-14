#!expect
[1] FALSE
[1] FALSE
[1] FALSE
[1] TRUE
[1] TRUE
#!end

# NaN comparisons: NaN is not equal to anything, including itself
nan <- 0 / 0
print(nan == nan)
print(nan < 1)
print(nan > 1)
print(1 == 1)
print(nan != nan)
