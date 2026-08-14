#!expect
[1] 55
#!end

total <- 0
for (i in 1:10) {
    total <- total + i
}
print(total)
