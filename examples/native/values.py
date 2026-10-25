def fibonacci(n):
    if n < 2:
        return n
    return fibonacci(n - 1) + fibonacci(n - 2)


print("Fibonacci:", fibonacci(25))
value = 9223372036854775807
print("Arbitrary precision:", value * value * value)
print("Strings:", "Ca" + "RT", "na" * 3)
for character in "hé😀":
    print(character)
