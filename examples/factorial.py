"""A native CIR example: arbitrary-precision arithmetic and ordinary functions."""


def factorial(n):
    result = 1
    while n > 1:
        result = result * n
        n = n - 1
    return result


for number in range(1, 31):
    print(number, factorial(number))
