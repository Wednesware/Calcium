import subprocess
import sys

import pytest

from calcium import Calcium


@pytest.mark.parametrize("level", [0, 1, 2, 3])
def test_definition_fallback_preserves_native_neighbors(tmp_path, level):
    source = tmp_path / "mixed.py"
    source.write_text("""events = []
def native(a, b):
    return a + b

def decorator(fn):
    events.append('decorate')
    return fn

@decorator
def closure_factory(value):
    events.append('factory')
    def inner():
        return value
    return inner

class Counter:
    events.append('class')
    def __init__(self, value): self.value = value
    def __add__(self, other): return self.value + other.value

def generator():
    for i in range(3):
        yield i

def exceptions():
    try:
        raise ValueError('bad')
    except ValueError:
        events.append('caught')

counter = Counter(closure_factory(7)())
exceptions()
print(counter + Counter(2), native(2, 3), list(generator()), events)
""")
    baseline = subprocess.run([sys.executable, str(source)], capture_output=True)
    result = Calcium(target="executable", fallback=True, optimization=level, keep_c=True).build(
        source, output=tmp_path / "program"
    )
    assert result.success, result.diagnostics
    assert result.metadata["mode"] == "mixed"
    assert all(reason["boundary"] == "definition" for reason in result.fallbacks)
    functions = {f["name"] for f in result.ir["functions"]}
    assert {"native", "decorator"} <= functions
    assert not {"closure_factory", "Counter", "generator", "exceptions"} & functions
    source.rename(tmp_path / "hidden.py")
    actual = subprocess.run([str(result.executable)], capture_output=True)
    assert (actual.returncode, actual.stdout, actual.stderr) == (
        baseline.returncode,
        baseline.stdout,
        baseline.stderr,
    )
    source = tmp_path / "hidden.py"
    strict = Calcium(fallback=False).analyze(source)
    assert not strict.success


def test_definition_evaluation_order_and_future_flags(tmp_path):
    source = tmp_path / "order.py"
    source.write_text("""from __future__ import annotations
print('before')
def f(a: missing = print('default')) -> missing:
    return a
print(f.__annotations__)
""")
    result = Calcium(target="executable", fallback=True).build(source)
    assert result.success, result.diagnostics
    assert result.metadata["mode"] == "fallback"
    assert subprocess.check_output([str(result.executable)]) == subprocess.check_output(
        [sys.executable, str(source)]
    )


def test_analysis_never_executes_fallback_definitions(tmp_path):
    source = tmp_path / "untrusted.py"
    marker = tmp_path / "marker"
    source.write_text(f"""class C:
    open({str(marker)!r}, 'w').write('bad')
""")
    result = Calcium(fallback=True).analyze(source)
    assert result.success
    assert not marker.exists()
