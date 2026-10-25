import hashlib
import json
import sqlite3
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from functools import partial
from pathlib import Path
from uuid import UUID


@dataclass
class Item:
    name: str
    count: int = 2
    values: list = field(default_factory=list)

    def update(self):
        self.values.append(self.count)


a = Item("hé😀")
a.update()
print(a, asdict(a), Item("other").values)
print(json.dumps(asdict(a)), json.dumps(asdict(a), ensure_ascii=False, indent=2))
print(json.loads('{"x":123456789012345678901234567890,"u":"\\ud83d\\ude00"}'))
print(hashlib.sha256(b"abc").hexdigest())
h = hashlib.sha256()
h.update(b"ab")
h2 = h.copy()
h.update(b"c")
print(h.hexdigest(), h2.hexdigest())
print(
    datetime.fromtimestamp(0, timezone.utc).isoformat(),
    str(UUID("00112233-4455-6677-8899-aabbccddeeff")),
)
print("ΟΣ ΟΣΑ İßé".lower(), "ΟΣ ΟΣΑ İßé".upper())
print(" hé😀 ".strip(), b"abc".decode(), "hé".encode(), "ab😀c".replace("", "-", 2))
print(" a  b ".split(), "a,b,c".rsplit(",", 1))
print(list({"x": 3, "y": 4}.items()))


def f(x, y=2):
    return x + y


p = partial(f, 3)
print(p(), p(y=4))
root = Path("workspace")
root.mkdir(parents=True, exist_ok=True)
file = root / "text.txt"
print(file.write_text("hé😀\n"), file.read_text(), file.name, file.suffix, file.parent == root)
db = sqlite3.connect(":memory:")
with db:
    db.execute("CREATE TABLE data (id INTEGER PRIMARY KEY, value TEXT)")
    db.execute("INSERT INTO data VALUES (?,?)", (1, "hé😀"))
print(db.execute("SELECT id,value FROM data").fetchone())
try:
    with db:
        db.execute("INSERT INTO data VALUES (?,?)", (2, "bad"))
        raise ValueError("rollback")
except ValueError:
    pass
print(db.execute("SELECT id,value FROM data ORDER BY id").fetchall())
db.close()

print(datetime(9999, 12, 31, 23, 59, 59, 123456, tzinfo=timezone.utc).isoformat())
print(datetime(1, 1, 1, 0, 0, 0, 999999, tzinfo=timezone.utc).isoformat())
