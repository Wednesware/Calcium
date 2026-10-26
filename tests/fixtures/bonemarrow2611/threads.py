import asyncio
import threading
import time

values = []
lock = threading.Lock()
ready = threading.Event()


def work(base):
    ready.wait()
    for i in range(100):
        with lock:
            values.append(base + i)
        time.sleep(0)


threads = [threading.Thread(target=work, args=(base,)) for base in [0, 100]]
for thread in threads:
    thread.start()
ready.set()
for thread in threads:
    thread.join()
print(len(values), sum(values), sorted(values) == list(range(200)))
print([t.is_alive() for t in threads], threading.active_count())
print(threading.current_thread().name)
print(lock.acquire(False), lock.locked())
lock.release()
print(lock.locked())
print(ready.is_set(), ready.wait(0))
ready.clear()
print(ready.wait(0))
recursive = threading.RLock()
with recursive:
    with recursive:
        print("recursive")


async def value():
    await asyncio.sleep(0)
    return 42


def runner():
    values.append(asyncio.run(value()))


thread = threading.Thread(target=runner, daemon=True)
thread.start()
thread.join()
print(values[-1])
try:
    thread.start()
except RuntimeError:
    print("cannot restart")
try:
    asyncio.get_running_loop()
except RuntimeError:
    print("no loop")


async def launch():
    loop = asyncio.get_running_loop()
    print(loop.is_running(), loop is asyncio.get_running_loop())
    thread = threading.Thread(target=runner)
    thread.start()
    thread.join()
    return loop, values[-1]


loop, result = asyncio.run(launch())
print(loop.is_running(), result)

# Finished thread wrappers and captured callback data may be collected before
# the native runtime reaps its remaining internal thread contexts.
threads = []
thread = None
for i in range(2000):
    cycle = []
    cycle.append(cycle)
