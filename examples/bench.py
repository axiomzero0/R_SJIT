#!/usr/bin/env python3
"""Python equivalent of bench.r for performance comparison."""

import sys
import time

# 1. Sum 1 to 1,000,000
s = 0
for i in range(1, 1000001):
    s += i
print(s)

# 2. Recursive Fibonacci
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
print(fib(25))

# 3. Nested loop
total = 0
for i in range(1, 501):
    for j in range(1, 501):
        total += i * j
print(total)
