"""Native 26.10 built-in and pattern-matching demonstration."""

values = list(map(lambda value: value * value, range(6)))
print(values)
print(any(value > 20 for value in values), all(value >= 0 for value in values))
first, *middle, last = reversed(values)
print(first, middle, last)
print(pow(3, -1, 11), divmod(23, 5), round(2.675, 2))
print(hex(2**80), chr(0x1F600))
match first:
    case 25 | 36:
        print("matched")
    case _:
        print("other")
