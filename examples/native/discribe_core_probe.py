"""Discribe core integration probe; run in a fresh disposable directory.

ca examples/native/discribe_core_probe.py --include-path /path/to/discribe
The clock/UUID are controlled for deterministic differential output.
"""

from pathlib import Path
from dataclasses import asdict
from datetime import datetime, timezone
from uuid import UUID
from discribe import core


class Clock:
    @staticmethod
    def now(tz):
        return datetime.fromtimestamp(0, timezone.utc)


core.datetime = Clock
core.uuid4 = lambda: UUID("00112233-4455-4677-8899-aabbccddeeff")
root = Path("workspace")
store = core.DocumentStore(root)
doc = core.Document(name="hello.py", content='print("hé😀")', id="doc")
value = asdict(doc)
saved = store.save(value)
print(saved["revision"], saved["createdAt"], saved["modifiedAt"], saved["content"])
print(store.list()[0]["id"])
try:
    store.save(value)
except core.ConflictError as error:
    print("conflict", isinstance(error, ValueError))
print(store.settings({"theme": "dark"}), store.settings())
file = root / "hello.py"
core.atomic_write(file, "hé😀\n", None)
print(core.language_for(file.name), core.digest(file), file.read_text())
read = core.read_document(file)
print(read.name, read.content, read.language, read.revision)
try:
    core.atomic_write(file, "bad", "stale")
except core.ConflictError:
    print("disk conflict")
store.delete(saved["id"], saved["revision"])
print(store.list())
