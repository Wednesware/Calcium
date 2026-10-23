"""An explicitly reported CPython fallback example, rather than native coverage."""

import asyncio


class Counter:
    def __init__(self, value):
        self.value = value

    def __add__(self, other):
        return Counter(self.value + other.value)


async def main():
    print((Counter(2) + Counter(3)).value)
    await asyncio.sleep(0)
    print([x * x for x in range(5)])


asyncio.run(main())
