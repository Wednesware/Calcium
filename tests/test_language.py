"""New syntax must execute generated C with the same effects as CPython."""

import shutil
import subprocess
import sys
import sysconfig

import pytest

from calcium import Calcium

COMPILERS = [name for name in ("gcc", "clang") if shutil.which(name)]

PROGRAMS = {
    "comparison": """
def mark(n):
    print('operand', n)
    return n
print(mark(1) < mark(2) < mark(3))
print(mark(5) < mark(2) < mark(3))
if mark(1) < mark(2) <= mark(2):
    print('yes')
if mark(5) < mark(2) < mark(3):
    print('wrong')
print(1 < 2 < 3 < 4, 1 < 2 > 3 < 4)
""",
    "unpacking": """
a, (b, c) = [1, (2, 3)]
print(a, b, c)
for x, y in [(1, 2), (3, 4)]:
    print(x, y)
[] = []
""",
    "formatting": """
def mark(value):
    print('eval', value)
    return value
n = 12.345
print(f'{mark(n):{mark(8)}.{mark(2)}f}')
print(f'{n=}', f'{"é"!a}', f'{[1, 2]!r:>12}', f'{n!s}', f'')
""",
    "assignment": """
from types import SimpleNamespace
x = SimpleNamespace(value=[1])
alias = x.value
x.value += [2]
a = [[1], [2]]
def target():
    print('target')
    return a
def key():
    print('key')
    return 1
target()[key()] += [3]
while (count := len(a)) > 0:
    print(count)
    a.pop()
print(alias, x.value, alias is x.value)
del x.value
d = {'a': 1, 'b': 2}
del d['a']
a, b = 1, 2
del a, b
print(d)
""",
    "signatures": """
def mark(value):
    print('default', value)
    return value
def f(a, /, b=mark(2), *args, c=mark(3), **kwargs):
    print(a, b, args, c, sorted(kwargs.items()))
f(1)
f(1, 4, 5, 6, c=7, a=8, extra=9)
def mutable(values=[]):
    values.append(len(values))
    return values
print(mutable(), mutable())
def keywords(*, required, optional=4):
    print(required, optional)
keywords(required=3)
def rest(**kw):
    return sorted(kw.items())
print(rest(a=1, b=2))
""",
    "globals": """
x = 1
def modify():
    global x
    x += 1
modify()
print(x)
def remove():
    global x
    del x
remove()
x = 3
print(x)
""",
    "starred_containers": """
a, *b, c = range(6)
print(a, b, c)
*a, = range(3)
print(a, [0, *a, 4], (*a, 5))
a, (b, *c) = [1, [2, 3, 4]]
print(a, b, c)
print({'x': 1, **{'y': 2}, 'x': 3, **{'z': 4}})
""",
    "assertions": """
def fail():
    print('wrong')
    return False
assert True, fail()
assert 1 < 2 < 3
print('ok')
""",
}


@pytest.mark.parametrize("compiler", COMPILERS)
@pytest.mark.parametrize("level", [0, 1, 2, 3])
@pytest.mark.parametrize("program", PROGRAMS.values(), ids=PROGRAMS.keys())
def test_language_differential(tmp_path, compiler, level, program):
    source = tmp_path / "program.py"
    source.write_text(program)
    result = Calcium(
        target="executable", fallback=False, compiler=compiler, optimization=level
    ).build(source)
    assert result.success, result.diagnostics
    assert result.metadata["mode"] == "native"
    expected = subprocess.run([sys.executable, str(source)], capture_output=True, timeout=10)
    actual = subprocess.run([str(result.executable)], capture_output=True, timeout=10)
    assert (actual.returncode, actual.stdout, actual.stderr) == (
        expected.returncode,
        expected.stdout,
        expected.stderr,
    )


@pytest.mark.parametrize(
    "program,exception",
    [
        ("assert False", "AssertionError"),
        ("assert False, ('one', 'two')", "AssertionError"),
        ("raise ValueError('bad') from TypeError('cause')", "ValueError"),
        ("raise", "RuntimeError"),
        ("raise 42", "TypeError"),
        ("a, b = [1]", "ValueError"),
        ("a, b = [1, 2, 3]", "ValueError"),
        ("del unknown", "NameError"),
        ("def f():\n    x = 1\n    del x\n    del x\nf()", "UnboundLocalError"),
        ("def f(a, /):\n    pass\nf(a=1)", "TypeError"),
        ("def f(*, a):\n    pass\nf()", "TypeError"),
        ("def f(a=1):\n    pass\nf(1, a=2)", "TypeError"),
    ],
)
def test_language_errors(tmp_path, program, exception):
    source = tmp_path / "errors.py"
    source.write_text(program)
    result = Calcium(target="executable", fallback=False).build(source)
    assert result.success, result.diagnostics
    expected = subprocess.run([sys.executable, str(source)], capture_output=True, timeout=10)
    actual = subprocess.run([str(result.executable)], capture_output=True, timeout=10)
    assert actual.returncode == expected.returncode == 1
    assert actual.stdout == expected.stdout
    assert actual.stderr.splitlines()[-1].split(b":")[0] == exception.encode()


def test_comparison_custom_result_truth_once(tmp_path):
    source = tmp_path / "chain.py"
    source.write_text("""def choose(a, b, c):
    print(a < b < c)
    if a < b < c:
        print('yes')
    else:
        print('no')
""")
    result = Calcium(target="extension", fallback=False).build(source)
    assert result.success, result.diagnostics
    runner = """import importlib.util, sys
spec = importlib.util.spec_from_file_location('chain', sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
class Result:
    def __init__(self, truth): self.truth = truth
    def __bool__(self): print('bool', self.truth); return self.truth
    def __repr__(self): return 'Result'
class Probe:
    def __init__(self, truth): self.truth = truth
    def __lt__(self, other): print('compare'); return Result(self.truth)
for truth in [False, True]: m.choose(Probe(truth), Probe(True), Probe(True))
"""
    extension = next(a.path for a in result.artifacts if a.kind == "extension")
    expected = subprocess.check_output([sys.executable, "-c", runner, str(source)])
    actual = subprocess.check_output([sys.executable, "-c", runner, extension])
    assert actual == expected


def test_argument_expansion(tmp_path):
    source = tmp_path / "expanded.py"
    source.write_text("""def mark(value):
    print('eval', value)
    return value
def f(a, /, b=2, *args, c=3, **kw):
    print(a, b, args, c, sorted(kw.items()))
f(*mark([1, 4]), *mark([5, 6]), **mark({'c': 7}), **mark({'a': 8, 'other': 9}))
print(*[1, 2, 3], sep=':')
""")
    result = Calcium(target="executable", fallback=False).build(source)
    assert result.success, result.diagnostics
    expected = subprocess.check_output([sys.executable, str(source)])
    actual = subprocess.check_output([str(result.executable)])
    assert actual == expected


@pytest.mark.parametrize(
    "call",
    [
        "f(**{'a': 1}, **{'a': 2})",
        "f(*42)",
        "f(**42)",
        "f(**{1: 2})",
    ],
)
def test_argument_expansion_errors(tmp_path, call):
    source = tmp_path / "badargs.py"
    source.write_text("def f(**kwargs):\n    print(kwargs)\n" + call)
    result = Calcium(target="executable", fallback=False).build(source)
    assert result.success, result.diagnostics
    actual = subprocess.run([str(result.executable)], capture_output=True)
    expected = subprocess.run([sys.executable, str(source)], capture_output=True)
    assert actual.returncode == expected.returncode == 1
    assert actual.stdout == expected.stdout
    assert actual.stderr.splitlines()[-1].startswith(b"TypeError:")


@pytest.mark.parametrize("level", [2, 3])
def test_checked_integer_overflow_and_subclass_dispatch(tmp_path, level):
    source = tmp_path / "numeric.py"
    source.write_text("""def operations(a, b):
    return a + b, a - b, a * b
for a in [-2**100, -2**63, -1, 0, 1, 2**63-1, 2**100]:
    for b in [-2**100, -2**63, -1, 0, 1, 2**63-1, 2**100]:
        print(operations(a, b))
print(operations(True, False), operations(1.5, 2.0))
""")
    result = Calcium(target="executable", fallback=False, optimization=level).build(source)
    assert result.success, result.diagnostics
    assert any(
        r["pass"] == "checked-integer-arithmetic" and r["operations"] == 3
        for r in result.optimizations
    )
    assert subprocess.check_output([str(result.executable)]) == subprocess.check_output(
        [sys.executable, str(source)]
    )
    extension = Calcium(target="extension", fallback=False, optimization=level).build(
        source, output=tmp_path / ("numeric" + sysconfig.get_config_var("EXT_SUFFIX"))
    )
    assert extension.success, extension.diagnostics
    runner = """import importlib.util, sys
spec = importlib.util.spec_from_file_location('numeric', sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
class Number(int):
    def __add__(self, other): return 'add'
    def __sub__(self, other): return 'subtract'
    def __mul__(self, other): return 'multiply'
assert m.operations(Number(2), 3) == ('add', 'subtract', 'multiply')
"""
    artifact = next(a.path for a in extension.artifacts if a.kind == "extension")
    subprocess.run([sys.executable, "-c", runner, artifact], check=True, capture_output=True)


def test_fstring_conversion_order_matches_python_minor(tmp_path):
    source = tmp_path / "format_order.py"
    source.write_text("""def format_value(value, specification):
    return f'{value!r:{specification()}}'
""")
    result = Calcium(target="extension", fallback=False).build(source)
    assert result.success, result.diagnostics
    runner = """import importlib.util, sys
spec = importlib.util.spec_from_file_location('format_order', sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
class Value:
    def __repr__(self): print('convert'); return 'x'
def specification(): print('spec'); return '>3'
print(m.format_value(Value(), specification))
"""
    output = next(a.path for a in result.artifacts if a.kind == "extension")
    expected = subprocess.check_output([sys.executable, "-c", runner, str(source)])
    actual = subprocess.check_output([sys.executable, "-c", runner, output])
    assert actual == expected


@pytest.mark.parametrize("level", [0, 1, 2, 3])
def test_nan_constants_preserve_value_sign_and_identity(tmp_path, level):
    source = tmp_path / "nan_values.py"
    source.write_text("""import math
a = 1e309 - 1e309
b = 1e309 - 1e309
c = -(1e309 - 1e309)
print(math.isnan(a), math.copysign(1.0, a), a is b, math.copysign(1.0, c))
def constant():
    return 1e309 - 1e309
print(constant() is constant())
a = (1e309 - 1e309,)
b = (1e309 - 1e309,)
print(a is b, a[0] is b[0])
print(1e309, -1e309, math.copysign(1.0, -0.0))
""")
    result = Calcium(target="executable", fallback=False, optimization=level).build(source)
    assert result.success, result.diagnostics
    assert subprocess.check_output([str(result.executable)]) == subprocess.check_output(
        [sys.executable, str(source)]
    )
