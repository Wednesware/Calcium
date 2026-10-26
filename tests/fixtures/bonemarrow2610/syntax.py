def choose(value):
    match value:
        case "a" | "b" if value == "b":
            return 1
        case "a":
            return 2
        case None:
            return 3
        case _:
            return 4


print([choose(v) for v in ["a", "b", None, 9]])
first, *middle, last = range(6)
print(first, middle, last)
[*only] = range(3)
print(only)
try:
    a, *b, c = []
except ValueError:
    print("unpack error")
for exception, parent in [
    (SyntaxError, Exception),
    (TabError, IndentationError),
    (ConnectionResetError, ConnectionError),
    (FloatingPointError, ArithmeticError),
    (OverflowError, ArithmeticError),
    (GeneratorExit, BaseException),
    (ResourceWarning, Warning),
]:
    print(issubclass(exception, parent), str(exception("bad")))
print(IOError is OSError, EnvironmentError is OSError)
