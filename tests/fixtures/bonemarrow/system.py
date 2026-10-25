from datetime import datetime, timezone
from secrets import token_hex, token_urlsafe
from time import monotonic, perf_counter, sleep
from uuid import UUID, uuid4

value = uuid4()
print(value.version == 4, UUID(str(value)) == value)
print(len(token_hex(8)) == 16, len(token_urlsafe(12)) == 16)
start = monotonic()
sleep(0.001)
print(monotonic() >= start, perf_counter() > 0)
print(datetime.now(timezone.utc).year >= 2026)
