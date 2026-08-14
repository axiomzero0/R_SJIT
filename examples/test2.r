# Test closures, loops, conditionals
add <- function(a, b) {
    a + b
}

result <- add(3, 4)
print(result)

# Loop
total <- 0
for (i in 1:100) {
    total <- total + i
}
print(total)

# Conditional
x <- 5
if (x > 3) {
    print(100)
} else {
    print(200)
}

# Nested calls
print(add(add(1, 2), add(3, 4)))
