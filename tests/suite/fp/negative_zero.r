#!expect
[1] TRUE
[1] FALSE
[1] FALSE
#!end

# -0 == 0, but they have different bit patterns
neg_zero <- -0.0
pos_zero <- 0.0
print(neg_zero == pos_zero)
print(neg_zero < pos_zero)
print(neg_zero > pos_zero)
