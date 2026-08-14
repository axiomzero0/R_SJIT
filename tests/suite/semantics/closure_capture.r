#!expect
[1] 1
[1] 2
[1] 3
#!end

# Closure capturing mutable state via super-assignment (<<-)
make_counter <- function() {
    x <- 0
    function() {
        x <<- x + 1
        x
    }
}
c <- make_counter()
print(c())
print(c())
print(c())
