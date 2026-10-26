events = []


def values():
    return (record(i) for i in range(5))


def record(i):
    events.append(i)
    return i


print(any(values()), events)
events.clear()
print(all(values()), events)
print(any([]), all([]))
m = map(lambda a, b: a + b, range(4), range(2))
print(events, next(m), list(m), list(m))
print(list(filter(None, [0, 1, False, "", 2])), list(filter(lambda n: n % 2, range(7))))
print(list(reversed("hé😀")), list(reversed((1, 2, 3))), list(reversed({"a": 1, "b": 2})))
print(list(reversed(range(10, 0, -3))))


def factory():
    i = 0

    def call():
        nonlocal i
        i += 1
        return i

    return call


it = iter(factory(), 4)
print(list(it), next(it, "finished"))


class Index:
    def __index__(self):
        return 65


print(chr(Index()), ord("😀"), ord(b"x"))
print(bin(Index()), hex(-(2**130)), oct(2**150))
print(isinstance(map(str, []), map), isinstance(filter(None, []), filter))
