import copy
import subprocess
import sys
import sysconfig

import pytest

from calcium import Calcium
from calcium.frontend.analysis import analyze
from calcium.frontend.loader import load
from calcium.ir.builder import build
from calcium.ir.validator import validate
from calcium.models import CalciumInternalError

SOURCE = """def squares(n):
    total = 0
    for i in range(n):
        total += i*i
    return total

def sweep(start, stop, step):
    total = 1000
    for i in range(start, stop, step):
        if i == 2:
            continue
        if i == 8:
            break
        total += i
    else:
        total += 10
    return total

def recurrence(n):
    if n < 2:
        return n
    return recurrence(n-1) + recurrence(n-2)

def multiply(a, b):
    return a * b + a - b

def unchanged(n):
    total = 1000
    while n > 0:
        total += n
        n -= 1
    return total

def identity(n):
    return +n
"""


@pytest.mark.parametrize("level", [2, 3])
def test_unboxed_kernels_differential_and_guards(tmp_path, level):
    source = tmp_path / "numeric.py"
    source.write_text(SOURCE)
    extension = tmp_path / ("numeric" + sysconfig.get_config_var("EXT_SUFFIX"))
    result = Calcium(
        target="extension", optimization=level, fallback=False, keep_c=True, cache=False
    ).build(source, output=extension)
    assert result.success, result.diagnostics
    assert any(r["pass"] == "unboxed-integer-region" for r in result.optimizations)
    generated = "\n".join(
        p.read_text() for p in extension.with_name(extension.name + ".calcium").glob("module_*.c")
    )
    assert "ca_kernel_" in generated and "long k0" in generated
    runner = """import importlib.util,sys
spec = importlib.util.spec_from_file_location('numeric',sys.argv[1])
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
for n in [0,1,10,1000]: print(m.squares(n),m.unchanged(n))
for args in [(0,10,1),(10,-8,-2),(-10,0,3),(0,0,1),(5,5,-1),(-2**63,-2**63+5,1),(2**63-1,2**63-8,-1),(-2**63,2**63-1,2**63-1)]: print(m.sweep(*args))
for n in range(18): print(m.recurrence(n))
for a in [-2**100, -2**63, -1,0,1,2**63-1,2**100]:
 for b in [-2**63,-1,0,1,2**63-1]: print(m.multiply(a,b))
x=int('1000000'); print(m.identity(x) is x, m.unchanged(0) is m.unchanged(0))
class IntSubclass(int):
 def __mul__(self, other): print('override'); return int(self)*other
print(m.multiply(IntSubclass(4),3))
import random
rng=random.Random(567)
for i in range(200): print(m.multiply(rng.randrange(-2**63,2**63),rng.randrange(-100,100)))
original_range=m.__dict__.get('range')
m.__dict__['range']=lambda n: [7,8]
print(m.squares(99))
del m.__dict__['range']
original=m.recurrence
m.recurrence=lambda n: 7
print(original(4))
try: m.sweep(0,10,0)
except Exception as e: print(type(e).__name__,str(e))
"""
    expected = subprocess.run(
        [sys.executable, "-c", runner, str(source)], capture_output=True, timeout=15
    )
    actual = subprocess.run(
        [sys.executable, "-c", runner, str(extension)], capture_output=True, timeout=15
    )
    assert (actual.returncode, actual.stdout, actual.stderr) == (
        expected.returncode,
        expected.stdout,
        expected.stderr,
    )


def test_effectful_function_is_not_specialized(tmp_path):
    source = tmp_path / "effects.py"
    source.write_text("def work(n):\n    print(n)\n    return n+1\n")
    result = Calcium(optimization=3, fallback=False).inspect(source)
    assert result.success
    assert not any(r["pass"] == "unboxed-integer-region" for r in result.optimizations)


def test_invalid_kernel_ir_is_rejected(tmp_path):
    source = tmp_path / "numeric.py"
    source.write_text(SOURCE)
    program = build(analyze(load(source)).tree)
    invalid = copy.deepcopy(program)
    invalid.functions[1].kernel["body"][0]["value"] = {"op": "unchecked-call"}
    with pytest.raises(CalciumInternalError, match="native kernel"):
        validate(invalid)
