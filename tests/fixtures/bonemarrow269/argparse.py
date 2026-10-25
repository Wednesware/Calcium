import argparse
from pathlib import Path

p = argparse.ArgumentParser(prog="tool", description="description")
p.add_argument("targets", nargs="*")
p.add_argument("--output", type=Path, default=Path("dist"))
p.add_argument("--port", type=int, default=8080)
p.add_argument("-v", "--verbose", action="store_true")
a = p.parse_intermixed_args(["a", "--port", "1234", "b", "--verbose", "--output=build"])
print(a.targets, a.port, a.verbose, str(a.output))
print(p.parse_args([]).targets, p.parse_args([]).port)
