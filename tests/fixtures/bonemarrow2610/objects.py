a = []
b = a
c = []
print(a is b, a is c, id(a) == id(b), id(a) != id(c))


def make():
    return 10**100


x = make()
y = x
print(x is y, id(x) == id(y))
x = float("nan")
y = x
print(x is y, hash(x) == hash(y), hash(1) == hash(True) == hash(1.0))
print(hash(-1), hash(2**100), hash(-(2**100)), hash(1.25), hash(float("inf")))
print(hash((1, 2, 3)), hash((1.0, 2, 3)) == hash((1, 2, 3)))


class Custom:
    def __init__(self):
        self.a = 1

    def __hash__(self):
        return 10**100


obj = Custom()
print(hash(obj) == hash(10**100), vars(obj))
vars(obj)["b"] = 2
delattr(obj, "a")
print(vars(obj), hasattr(obj, "a"))


class Equal:
    def __eq__(self, other):
        return True


try:
    hash(Equal())
except TypeError:
    print("unhashable")
print(type([]) is list, type([]) == list, isinstance(range(1), range))  # noqa: E721
s = slice(1, 7, 2)
print(s.start, s.stop, s.step, [0, 1, 2, 3, 4, 5, 6, 7][s], isinstance(s, slice))


class Parent:
    def __hash__(self):
        return 7


class Child(Parent):
    def __eq__(self, other):
        return True


try:
    hash(Child())
except TypeError:
    print("hash disabled")
