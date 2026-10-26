import os
from pathlib import Path

with open("text.txt", "w", encoding="utf-8") as f:
    print(f.write("hé😀\r\nsecond\nlast"), f.closed)
print(f.closed)
with open(Path("text.txt"), encoding="utf-8") as f:
    print(f.readline(), f.read(3), f.read())
    f.seek(0)
    print(list(f))
with open("binary.dat", "xb") as f:
    f.writelines([b"ab", b"\x00\xff"])
with open("binary.dat", "rb+") as f:
    print(f.read(), f.tell())
    f.seek(1)
    f.write(b"Z")
    f.seek(0)
    print(f.read())
try:
    open("binary.dat", "x")
except FileExistsError:
    print("exclusive")
with open("text.txt", encoding="ascii", errors="replace") as f:
    print(repr(f.read()))
with open("text.txt", encoding="utf-8") as f:
    print(f.readlines(4))

fd = os.open("binary.dat", os.O_RDONLY)
with open(fd, "rb", closefd=False) as f:
    print(f.read(2))
with os.fdopen(fd, "rb") as f:
    print(f.read())
