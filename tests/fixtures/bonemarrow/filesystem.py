import hashlib
import os
import shutil
import tempfile
from pathlib import Path

root = Path("tree")
(root / "child").mkdir(parents=True)
(root / "a.txt").write_bytes(b"a")
(root / "child" / "b.txt").write_bytes(b"b")
paths = sorted(p for p in root.rglob("*") if p.is_file())
print([p.as_posix() for p in paths])
print(hashlib.sha256(b"".join(p.read_bytes() for p in paths)).hexdigest())
shutil.copytree(root, Path("copy"), dirs_exist_ok=True)
print((Path("copy") / "child" / "b.txt").read_text())
fd, name = tempfile.mkstemp(dir=".", prefix="own-")
with os.fdopen(fd, "w", encoding="utf-8") as stream:
    stream.write("hello")
print(Path(name).read_text())
os.unlink(name)
shutil.rmtree(root)
print(root.exists())

fd, name = tempfile.mkstemp(dir=".", prefix="encoding-")
try:
    with os.fdopen(fd, "w", encoding="ascii") as stream:
        stream.write("hé")
except UnicodeEncodeError:
    print("encoding respected")
os.unlink(name)
