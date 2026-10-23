"""Differential tests compile real generated C and execute it, not mock emitters."""

import copy
import json
import shutil
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

COMPILERS = [name for name in ("gcc", "clang") if shutil.which(name)]
PROGRAMS = {
    "integer": """
def factorial(n):
    product = 1
    while n > 1:
        product = product * n
        n = n - 1
    return product
print(factorial(n=60), 2 ** 200, -9 // 4, -9 % 4, 1 << 100)
""",
    "calls": """
def show(value):
    print(value)
    return value
print(show(1) + show(2), show(3) < show(4))
def add(a, b):
    return a + b
alias = add
add = print
print(alias(b=7, a=2))
""",
    "containers": """
a = [1, 2, 3]
a[1] = 9
a.append(4)
print(a, a[1:], (a[0], a[1]), {'name': 'Ada', 'n': 2}, sorted({1, 2, 1}))
print(9 in a, 5 not in a, b'abc'[1:], 'nul\\x00é\\ud800'.encode('utf-8', 'surrogatepass'))
""",
    "short_circuit": """
def mark(x):
    print(x)
    return x
print(mark(0) and mark(9), mark(1) or mark(8))
print(mark(2) if mark(True) else mark(3))
print((mark(0) or mark(1)) and mark(2))
""",
    "loops": """
total = 0
for i in range(8):
    if i == 2:
        continue
    if i == 6:
        break
    total = total + i
else:
    print('wrong')
print(total)
for x in []:
    print('wrong')
else:
    print('empty')
i = 0
while i < 3:
    i = i + 1
else:
    print('done', i)
for a in range(3):
    for b in range(2):
        print(a, b)
""",
    "identity": """
a = 1000
b = 1000
print(a is b)
a = -1000
b = -1000
print(a is b)
a = (1000, 'hello world')
b = (1000, 'hello world')
print(a is b, a[0] is b[0])
def constant():
    return 1000
print(constant() is constant(), constant() is b[0])
print(1 is True, 0.0 is -0.0)
""",
    "recursion": """
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
print(fib(12))
""",
    "binding": """
x = 9
def read(flag):
    if flag:
        x = 2
    return x
print(read(True))
""",
    "operators": """
print(6 / 4, 6 // 4, 6 % 4, 2 ** 20, 3 & 5, 3 | 5, 3 ^ 5, 32 >> 2, ~4, not [], -2, +3)
print(None is None, [] is not [], 'a' == 'a', 2 != 3, 2 <= 2, 3 >= 2)
""",
}


@pytest.mark.parametrize("compiler", COMPILERS)
@pytest.mark.parametrize("level", [0, 1])
@pytest.mark.parametrize("name", PROGRAMS)
def test_native_differential(tmp_path, compiler, level, name):
    source = tmp_path / f"{name}.py"
    source.write_text(PROGRAMS[name])
    result = Calcium(
        target="executable", compiler=compiler, optimization=level, fallback=False
    ).build(source)
    assert result.success, result.diagnostics
    assert result.metadata["mode"] == "native"
    baseline = subprocess.run([sys.executable, str(source)], capture_output=True, timeout=10)
    native = subprocess.run([str(result.executable)], capture_output=True, timeout=10)
    # Ignore baseline SyntaxWarnings for intentional literal-identity comparisons.
    assert native.returncode == baseline.returncode == 0
    assert native.stdout == baseline.stdout
    assert native.stderr == b""


@pytest.mark.parametrize(
    "source_text,exception",
    [
        ("print(1 / 0)", "ZeroDivisionError"),
        ("def f():\n    return x\n    x = 2\nf()", "UnboundLocalError"),
        ("print(unknown_name)", "NameError"),
        ("print([1][3])", "IndexError"),
        ("def f(a):\n    return a\nf(1, a=2)", "TypeError"),
        ("def f():\n    return f()\nf()", "RecursionError"),
    ],
)
def test_native_errors(tmp_path, source_text, exception):
    source = tmp_path / "errors.py"
    source.write_text(source_text)
    result = Calcium(target="executable", fallback=False).build(source)
    assert result.success, result.diagnostics
    run = subprocess.run([str(result.executable)], capture_output=True, timeout=10)
    baseline = subprocess.run([sys.executable, str(source)], capture_output=True, timeout=10)
    assert run.returncode == baseline.returncode == 1
    assert run.stdout == baseline.stdout
    assert run.stderr.decode().splitlines()[-1].startswith(exception + ":")


@pytest.mark.parametrize("compiler", COMPILERS)
@pytest.mark.parametrize("level", [0, 1])
def test_dynamic_fallback(tmp_path, compiler, level):
    source = tmp_path / "dynamic.py"
    source.write_text("""import asyncio
class Number:
    def __init__(self, n): self.n = n
    def __add__(self, other): return self.n + other.n
async def run():
    print(Number(2) + Number(3))
    print([x * x for x in range(6)])
asyncio.run(run())
""")
    result = Calcium(target="executable", compiler=compiler, optimization=level).build(source)
    assert result.success and result.fallbacks
    assert result.metadata["mode"] == "fallback"
    baseline = subprocess.run([sys.executable, str(source)], capture_output=True, timeout=10)
    run = subprocess.run([str(result.executable)], capture_output=True, timeout=10)
    assert (run.returncode, run.stdout, run.stderr) == (
        baseline.returncode,
        baseline.stdout,
        baseline.stderr,
    )
    rejected = Calcium(fallback=False).analyze(source)
    assert not rejected.success and rejected.diagnostics[0].code == "C101"


def test_no_execution_and_scope_validation(tmp_path):
    source = tmp_path / "untrusted.py"
    marker = tmp_path / "marker"
    source.write_text(f"open({str(marker)!r}, 'w').write('executed')\n")
    assert Calcium().analyze(source).success
    assert not marker.exists()
    source.write_text("return 1\n")
    result = Calcium().analyze(source)
    assert not result.success and result.diagnostics[0].code == "C100"
    source.write_text("if :\n")
    assert not Calcium().transpile(source).success


def test_c_emission_reproducible(tmp_path, monkeypatch):
    source = tmp_path / "simple.py"
    source.write_text("print(2 + 3)\n")
    monkeypatch.setattr(
        "calcium.api.compile_c", lambda *a, **k: pytest.fail("transpile invoked compiler")
    )
    compiler = Calcium(emit_ir=True, emit_ast=True)
    a, b = tmp_path / "a", tmp_path / "b"
    assert compiler.transpile(source, output=a).success
    assert compiler.transpile(source, output=b).success
    assert {p.name: p.read_bytes() for p in a.iterdir()} == {
        p.name: p.read_bytes() for p in b.iterdir()
    }
    assert json.loads((a / "manifest.json").read_text())["mode"] == "native"
    assert compiler.transpile(source, output=a).success


def test_ir_invalid_ownership_and_error_edge(tmp_path):
    source = tmp_path / "simple.py"
    source.write_text("print(1)")
    program = build(analyze(load(source)).tree)
    validate(program)
    invalid = copy.deepcopy(program)
    invalid.functions[0].blocks[0].instructions[0].error = None
    with pytest.raises(CalciumInternalError, match="exception edge"):
        validate(invalid)
    invalid = copy.deepcopy(program)
    next(op for op in invalid.functions[0].blocks[0].instructions if op.op == "drop").args = [9999]
    with pytest.raises(CalciumInternalError, match="unowned"):
        validate(invalid)


@pytest.mark.parametrize("fallback", [False, True])
def test_extension_and_reference_lifetimes(tmp_path, fallback):
    source = tmp_path / "extension.py"
    source.write_text(
        ("import math\n" if fallback else "")
        + """
def identity(x):
    return x
def work(n):
    values = []
    for i in range(n):
        values.append(i)
    return len(values)
"""
    )
    suffix = sysconfig.get_config_var("EXT_SUFFIX")
    output = tmp_path / ("compiled" + suffix)
    result = Calcium(target="extension").build(source, output=output)
    assert result.success, result.diagnostics
    script = """import importlib.util, sys, gc
spec = importlib.util.spec_from_file_location('compiled', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
value = []
refs = sys.getrefcount(value)
for _ in range(10000):
    assert module.identity(value) is value
assert sys.getrefcount(value) == refs
assert module.work(10000) == 10000
print('ok')
"""
    run = subprocess.run(
        [sys.executable, "-c", script, str(output)], capture_output=True, timeout=10
    )
    assert run.returncode == 0, run.stderr
    assert run.stdout == b"ok\n"


def test_encoding_imports_and_argv(tmp_path):
    (tmp_path / "helper.py").write_text("value = 'dependency'\n")
    source = tmp_path / "latin.py"
    source.write_bytes(
        "# coding: latin-1\nimport helper, sys\nprint('café', helper.value, sys.argv[1:])\n".encode(
            "latin-1"
        )
    )
    result = Calcium(target="executable").build(source, output=tmp_path / "program")
    assert result.success, result.diagnostics
    baseline = subprocess.run([sys.executable, str(source), "one", "-x"], capture_output=True)
    run = subprocess.run([str(result.executable), "one", "-x"], capture_output=True)
    assert (run.returncode, run.stdout, run.stderr) == (
        baseline.returncode,
        baseline.stdout,
        baseline.stderr,
    )


def test_build_failures_and_output_protection(tmp_path):
    source = tmp_path / "source.py"
    source.write_text("print(1)")
    with pytest.raises(ValueError):
        Calcium(optimization=3)
    with pytest.raises(ValueError):
        Calcium(target="shared")
    with pytest.raises(ValueError):
        Calcium().transpile(source, output=tmp_path)
    output = tmp_path / "occupied"
    output.mkdir()
    sentinel = output / "mine.txt"
    sentinel.write_text("keep")
    with pytest.raises(ValueError):
        Calcium().transpile(source, output=output)
    assert sentinel.read_text() == "keep"
    result = Calcium(target="executable", compiler="does-not-exist").build(source)
    assert not result.success and result.diagnostics[-1].code == "C302"
    assert not Calcium(python="/does/not/exist").analyze(source).success


def test_path_quoting(tmp_path):
    source = tmp_path / 'quote " $` space.py'
    source.write_text("print('safe')")
    result = Calcium(target="executable").build(source, output=tmp_path / "native program")
    assert result.success, result.diagnostics
    assert subprocess.check_output([str(result.executable)]) == b"safe\n"


def test_flow_inference_and_unreachable_optimization(tmp_path):
    source = tmp_path / "facts.py"
    source.write_text("""x = 1
if False:
    x = 'text'
y = x
def f(flag):
    if flag:
        return 1
    else:
        return 2
print(f(True), f(False))
""")
    result = Calcium(optimization=1, fallback=False).inspect(source)
    assert result.success
    facts = result.analysis["types"]
    assert next(f for f in facts if f.get("name") == "y")["types"] == ["int", "str"]
    assert any(report["removed"] > 0 for report in result.optimizations)


def test_runtime_arithmetic_generated_corpus(tmp_path):
    import random

    rng = random.Random(103)
    expressions = []
    for _ in range(80):
        a, b = rng.randrange(-(10**30), 10**30), rng.randrange(1, 10**10)
        operation = rng.choice(["+", "-", "*", "//", "%", "&", "|", "^"])
        expressions.append(f"print(calc({a}, {b}, {repr(operation)}))")
    source = tmp_path / "corpus.py"
    branches = "\n".join(
        f"    if op == '{op}':\n        return a {op} b"
        for op in ["+", "-", "*", "//", "%", "&", "|", "^"]
    )
    source.write_text("def calc(a, b, op):\n" + branches + "\n" + "\n".join(expressions))
    result = Calcium(target="executable", optimization=1, fallback=False).build(source)
    assert result.success, result.diagnostics
    assert subprocess.check_output([str(result.executable)]) == subprocess.check_output(
        [sys.executable, str(source)]
    )


def test_identifier_string_interning(tmp_path):
    source = tmp_path / "intern.py"
    source.write_text("name = 'int'\nprint(int.__name__ is name)\n")
    result = Calcium(target="executable", fallback=False).build(source)
    assert result.success
    assert subprocess.check_output([str(result.executable)]) == subprocess.check_output(
        [sys.executable, str(source)]
    )


def test_augmented_assignment_aliasing(tmp_path):
    source = tmp_path / "inplace.py"
    source.write_text("""a = []
b = a
a += [1]
x = 2
x **= 100
for i in range(10):
    x += i
print(a, b, a is b, x)
""")
    result = Calcium(target="executable", fallback=False).build(source)
    assert result.success, result.diagnostics
    assert subprocess.check_output([str(result.executable)]) == subprocess.check_output(
        [sys.executable, str(source)]
    )


def test_short_circuit_custom_truth_protocol(tmp_path):
    source = tmp_path / "truth.py"
    source.write_text("""def choose(a, b, c):
    if a and b:
        print('yes')
    else:
        print('no')
    print((a and b) or c)
    print((a or b) and c)
    print(not (a and b))
    print((not (a and b)) and c)
    print(((a or b) if c else (b and a)) and c)
    if ((a or b) if c else (b and a)):
        print('selected')
""")
    result = Calcium(target="extension", fallback=False).build(source)
    assert result.success, result.diagnostics
    runner = """import importlib.util, sys
spec = importlib.util.spec_from_file_location(sys.argv[1], sys.argv[2])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
class Probe:
    def __init__(self, name, truth): self.name, self.truth = name, truth
    def __bool__(self): print('truth', self.name); return self.truth
    def __repr__(self): return self.name
for a in [False, True]:
    for b in [False, True]:
        module.choose(Probe('a', a), Probe('b', b), Probe('c', True))
"""
    output = next(a.path for a in result.artifacts if a.kind == "extension")
    baseline = subprocess.check_output([sys.executable, "-c", runner, "truth", str(source)])
    native = subprocess.check_output([sys.executable, "-c", runner, "truth", output])
    assert native == baseline


def test_native_normal_return_finalizer_order(tmp_path):
    source = tmp_path / "lifetime.py"
    source.write_text("""def work(make):
    z = make('z')
    a = make('a')
    if True:
        middle = make('middle')
    b = make('b')
""")
    result = Calcium(target="extension", fallback=False).build(source)
    assert result.success, result.diagnostics
    output = next(a.path for a in result.artifacts if a.kind == "extension")
    runner = """import importlib.util, sys
spec = importlib.util.spec_from_file_location('lifetime', sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
events = []
class Item:
    def __init__(self, name): self.name = name
    def __del__(self): events.append(self.name)
m.work(Item)
print(events)
"""
    baseline = subprocess.check_output([sys.executable, "-c", runner, str(source)])
    native = subprocess.check_output([sys.executable, "-c", runner, output])
    assert native == baseline
