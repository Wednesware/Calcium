import json
import sqlite3

for text in [
    "null",
    "true",
    "123456789012345678901234567890",
    "[1,2,3]",
    '"hé😀"',
    '{"a":1,"a":2}',
]:
    print(json.dumps(json.loads(text), ensure_ascii=False, sort_keys=True))
try:
    json.loads("{")
except json.JSONDecodeError as e:
    print(isinstance(e, ValueError))
a = []
a.append(a)
try:
    json.dumps(a)
except ValueError:
    print("cycle rejected")
db = sqlite3.connect(":memory:")
db.execute("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT UNIQUE)")
with db:
    db.execute("INSERT INTO t VALUES (?,?)", (1, "one"))
try:
    with db:
        db.execute("INSERT INTO t VALUES (?,?)", (2, "one"))
except sqlite3.IntegrityError:
    print("integrity")
print(db.execute("SELECT * FROM t").fetchall())
db.close()
try:
    db.execute("SELECT 1")
except sqlite3.ProgrammingError:
    print("closed")
