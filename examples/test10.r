count <- function(n) {
    if (n <= 0) {
        0
    } else {
        1 + count(n - 1)
    }
}
print(count(3))
print(count(5))
