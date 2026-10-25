import asyncio


class Context:
    async def __aenter__(self):
        await asyncio.sleep(0)
        print("enter")
        return 7

    async def __aexit__(self, kind, error, tb):
        await asyncio.sleep(0)
        print("exit", str(error))
        return kind is not None


async def slow():
    try:
        print("started")
        await asyncio.sleep(10)
    finally:
        await asyncio.sleep(0)
        print("finished")


async def main():
    async with Context() as value:
        print(value)
        raise ValueError("handled")
    try:
        raise ValueError("outer")
    except ValueError:
        await asyncio.sleep(0)
        try:
            raise
        except ValueError as error:
            print(str(error))
    try:
        await asyncio.wait_for(slow(), 0)
    except TimeoutError:
        print("immediate timeout")
    task = asyncio.create_task(slow())
    await asyncio.sleep(0)
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        print("cancelled after cleanup")

    async def value():
        return 8

    coro = value()
    print(await asyncio.gather(coro, coro))
    return 3


print(asyncio.run(main()))
