#!expect
[1] 5050
#!end

# Loop that should get JIT-compiled
total <- 0
for (i in 1:100) {
    total <- total + i
}
print(total)
