print(pow(2, 100), pow(2, -3), pow(10, 100, 97), pow(3, -2, 11), pow(3, -2, -11))
print(pow(base=5, exp=117, mod=19))
for a in [-20, -7, 0, 7, 20]:
    for b in [-4, 3]:
        print(divmod(a, b), divmod(float(a), float(b)))
for value in [2.5, 3.5, -2.5, -3.5, 2.675, 1.005, -0.0, 1e20, 1e-320]:
    print(round(value), round(value, 0), round(value, 2), round(value, -2))
print(round(10**100 + 51, -2), round(10**100 + 50, -2), round(-(10**100) - 150, -2))
print(round(1.234, 10000), round(-1.234, -10000))
try:
    pow(2, -1, 4)
except ValueError:
    print("not invertible")
try:
    round(float("nan"))
except ValueError:
    print("nan")
try:
    round(float("inf"))
except OverflowError:
    print("infinity")
print(format(12.345, ".2f"), format("ab", ">5"))
