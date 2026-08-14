# Pure loop benchmark — isolates interpreter dispatch overhead
s <- 0
for (i in 1:1000000) {
    s <- s + i
}
print(s)
