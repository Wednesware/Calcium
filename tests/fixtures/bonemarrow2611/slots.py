from dataclasses import asdict, dataclass


@dataclass(slots=True)
class Result:
    status: str
    lines: list[str]
    exit_code: int = 0


result = Result("ok", ["native"])
print(result, asdict(result), Result.__slots__)
print(hasattr(result, "__dict__"))
result.exit_code = 1
print(result.exit_code)
try:
    result.extra = 1
except AttributeError:
    print("slots enforced")
try:
    vars(result)
except TypeError:
    print("no dict")
