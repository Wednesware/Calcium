events = []


def source():
    events.append("source")
    return range(4)


def work(i):
    events.append(i)
    return i * i


g = (work(i) for i in source() if i != 1)
print(events)
print(next(g), events)
print(g.send(None), events)
print(list(g), events, next(g, "done"))
print(sum(x * x for x in range(100)))


def factory(x):
    return (x + i for i in range(3))


print(list(factory(3)))
funcs = [lambda: i for i in range(4)]
print([f() for f in funcs])
seq = [(lambda: (i for x in range(2))) for i in range(3)]
print([list(f()) for f in seq])
print("".join(str(i) for i in range(5)), b"".join(bytes([i]) for i in range(5)))

print(list(enumerate("abc", start=99999999999999999999999)))
print(list(zip(range(4), "abc")), list(zip()))
