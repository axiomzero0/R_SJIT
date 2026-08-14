#!expect
[1] 30
[1] 30
[1] 20
#!end

# Nested function scopes
outer <- function() {
    x <- 10
    inner <- function() {
        y <- 20
        x + y
    }
    z <- inner()
    z
}
print(outer())

# Deeper nesting
f1 <- function(a) {
    f2 <- function(b) {
        f3 <- function(c) {
            a + b + c
        }
        f3(10)
    }
    f2(10)
}
print(f1(10))

# Closure capturing from multiple levels
make_adder <- function(x) {
    function(y) {
        function(z) {
            x + y + z
        }
    }
}
add10 <- make_adder(10)
add20 <- add10(10)
print(add20(0))
