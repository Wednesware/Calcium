"""Separate numerical kernels, recursive calls, and boxed container work.

Run with:
    ca examples/compute.py --benchmark --function sum_squares --arguments '[1000000]' -O3
    ca examples/compute.py --benchmark --function fibonacci --arguments '[25]' -O3
    ca examples/compute.py --benchmark --function containers --arguments '[20000]' -O3
"""


def sum_squares(n):
    total = 0
    for i in range(n):
        total += i * i
    return total


def fibonacci(n):
    if n < 2:
        return n
    return fibonacci(n - 1) + fibonacci(n - 2)


def containers(n):
    values = [i * i for i in range(n)]
    counts = {i % 17: 0 for i in range(n)}
    for value in values:
        counts[value % 17] += 1
    return counts
