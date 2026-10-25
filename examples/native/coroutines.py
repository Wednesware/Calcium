"""Independent native coroutine frames, cooperative tasks and FIFO locks."""

import asyncio


async def worker(value, lock, events):
    async with lock:
        events.append(value)
        await asyncio.sleep(0)
        events.append(value + 10)
    return value * value


async def main():
    events = []
    lock = asyncio.Lock()
    values = await asyncio.gather(*(worker(i, lock, events) for i in range(3)))
    print(values)
    print(events)


asyncio.run(main())
