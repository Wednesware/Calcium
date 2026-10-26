import re

identifier = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
number = re.compile(r"(?:\d+\.?\d*|\.\d)(?:[eE][+-]?\d+)?")
print(identifier.match("x _abc42!", 2).group())
print(number.match("-.123E+4", 1).group())
print(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", "valid_name") is not None)
print(re.sub(r"[^A-Za-z0-9_.-]+", "-", " a:b? c! ".strip()))
print(re.match(r"([A-Za-z]*)(.*)", "Calcium26.11").groups())
m = re.search(r"(?P<word>\w+)-(\d+)(x)?", "😀hé-42!")
print(m.group(), m.group(1, 2), m.groups("missing"), m.groupdict())
print(m.start(), m.end(), m.span(1), m.span(3))
print(m.expand(r"\g<word>:\2"))
print(re.findall(r"(a)|(b)", "abba"))
print(re.findall(r"|a", "a"))
print([m.span() for m in re.finditer("a*", "baa")])
print(re.split(r"(\d+)", "a12b3c"))
print(re.subn(r"(\w+)", r"[\1]", "hé a", count=1))
print(re.sub("a", lambda m: m.group().upper(), "abac"))
print(re.compile("a", re.I).fullmatch("A").group())
print(re.match(b"[a-z]+", b"abc!").group())
print(re.escape("a.b+c? [x]"))
try:
    re.compile("[")
except re.error:
    print("invalid pattern")
print(re.compile("a*").match("aa", 0, -1).span())
print(re.sub("a", "b", "aaa", count=-1))
try:
    re.sub("a", "b", "a", count="1")
except TypeError:
    print("invalid count")
try:
    re.compile("a", re.A | re.U)
except ValueError:
    print("incompatible flags")
try:
    m.group(2**40)
except IndexError:
    print("invalid group")


class Position:
    def __index__(self):
        return 1


print(re.compile(".").match("xé", Position()).span())
