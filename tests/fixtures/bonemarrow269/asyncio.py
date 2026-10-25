import asyncio
from contextlib import nullcontext, suppress


async def work(i, lock, events):
    async with lock:
        events.append(i)
        await asyncio.sleep(0)
        events.append(i + 10)
    return i * 2


async def failing():
    await asyncio.sleep(0)
    raise ValueError("bad")


async def delayed():
    try:
        await asyncio.sleep(10)
    finally:
        print("cleanup")


async def main():
    events = []
    lock = asyncio.Lock()
    tasks = [asyncio.create_task(work(i, lock, events)) for i in range(3)]
    print(await asyncio.gather(*tasks), events, lock.locked())
    print(await asyncio.gather(failing(), return_exceptions=True))
    try:
        await asyncio.wait_for(delayed(), 0.005)
    except TimeoutError:
        print("timeout")
    task = asyncio.create_task(delayed())
    await asyncio.sleep(0)
    print(task.cancel())
    with suppress(asyncio.CancelledError):
        await task
    print(task.done())
    return 9


print(asyncio.run(main()))
with nullcontext(7) as x:
    print(x)
