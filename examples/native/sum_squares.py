def sum_squares(n: int) -> int:
    total = 0
    for i in range(n):
        total += i * i
    return total


print(sum_squares(1_000_000))
