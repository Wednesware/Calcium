import os
from pathlib import Path
from secrets import compare_digest

root = Path("sessions")
root.mkdir()
file = root / "one.json"
temp = file.with_suffix(".tmp")
fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
with os.fdopen(fd, "wb") as stream:
    stream.write(b'{"a":1}')
    stream.flush()
    os.fsync(stream.fileno())
os.replace(temp, file)
print(file.read_text(), file.stat().st_mode & 0o777)
print(sorted(p.name for p in root.glob("*.json")))
print(compare_digest("abc", "abc"), compare_digest("abc", "abcd"), compare_digest(b"abc", b"abd"))
try:
    compare_digest("é", "é")
except TypeError:
    print("ascii required")
data = {"a": 1, "b": 2}
print(data.pop("a"), data.pop("missing", None), data)
file.unlink()
file.unlink(missing_ok=True)
try:
    file.unlink()
except FileNotFoundError:
    print("missing")
