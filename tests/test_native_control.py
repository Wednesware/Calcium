"""Native structured cleanup and expression protocols, never source fallback."""

import subprocess
import sys

import pytest

from calcium import Calcium

PROGRAMS = {
    "handlers": """import sys
for value in [0, 2, 'bad']:
    try:
        print('try', value)
        result = 10 // value
    except ZeroDivisionError as error:
        print('zero', type(error).__name__, sys.exception() is error)
    except (TypeError, ValueError):
        print('type', type(sys.exception()).__name__)
    else:
        print('else', result)
    finally:
        print('finally')
print(sys.exception())
""",
    "nested": """import sys
try:
    raise ValueError('outer')
except ValueError as error:
    print('outer', str(sys.exception()))
    try:
        raise TypeError('inner')
    except TypeError as inner:
        print(str(inner), str(inner.__context__), sys.exception() is inner)
        del inner
    print(sys.exception() is error)
print(sys.exception())
""",
    "returns": """def work(n):
    try:
        try:
            if n == 0:
                return 1
            if n == 1:
                raise ValueError('v')
            return 3
        except ValueError as error:
            return str(error)
        finally:
            print('inner', n)
            if n == 2:
                return 8
    finally:
        print('outer', n)
for n in range(3):
    print('result', work(n))
""",
    "loop_cleanup": """for x in range(4):
    try:
        if x == 0:
            continue
        if x == 2:
            break
        print(x)
    finally:
        print('cleanup', x)
else:
    print('wrong')
for a in range(2):
    try:
        for b in range(2):
            if b:
                break
            print(a, b)
    finally:
        print('outer loop', a)
""",
    "overrides": """import sys
try:
    try:
        raise ValueError('first')
    finally:
        print(str(sys.exception()))
        raise TypeError('second')
except TypeError as error:
    print(str(error), str(error.__context__))
try:
    try:
        raise ValueError('x')
    except ValueError as error:
        del error
        raise TypeError('y')
except TypeError as error:
    print(str(error), str(error.__context__))
""",
    "reraising": """import sys
try:
    try:
        raise ValueError('message')
    except ValueError:
        print('caught')
        raise
except Exception as error:
    print(type(error).__name__, str(error))
try:
    raise
except RuntimeError:
    print('no active')
""",
    "with_files": """import tempfile, pathlib
with tempfile.TemporaryDirectory() as directory:
    with open(directory + '/a', 'w') as stream:
        stream.write('hello')
    print(stream.closed)
    with open(directory + '/a') as stream:
        print(stream.read())
    print(stream.closed)
""",
    "with_contextlib": """import contextlib, sys
with contextlib.suppress(ValueError):
    raise ValueError('suppressed')
print('after', sys.exception())
def run():
    with contextlib.nullcontext(7) as value:
        return value
print(run())
""",
    "comprehensions": """x = 'global'
def run():
    print([x*y for x in range(4) if x for y in range(3) if y != 1])
    print(x)
    print({x: x*x for x in range(5)})
    print(sorted({x%3 for x in range(9)}))
    y = 'outer'
    print([[y for y in range(x)] for x in range(4)])
    print(y)
    z = 0
    print([z := x+1 for x in range(4)], z)
run()
print(x)
""",
    "containers": """def key(n):
    print('key', n)
    return n
def value(n):
    print('value', n)
    return n*2
print({key(1): value(1), key(2): value(2)})
print({key(3): 8, **{4: 9}, key(5): 10})
print(sorted({1, *[2, 3], 4, 5, *range(3)}))
""",
}


@pytest.mark.parametrize("level", [0, 1, 2, 3])
@pytest.mark.parametrize("name", PROGRAMS)
def test_native_control(tmp_path, level, name):
    source = tmp_path / (name + ".py")
    source.write_text(PROGRAMS[name])
    baseline = subprocess.run(
        [sys.executable, "-Wignore::SyntaxWarning", str(source)], capture_output=True, timeout=10
    )
    assert baseline.returncode == 0, baseline.stderr
    result = Calcium(
        backend="cpython", target="executable", optimization=level, fallback=False, cache=False
    ).build(source, output=tmp_path / "program")
    assert result.success, result.diagnostics
    assert result.metadata["mode"] == "native"
    actual = subprocess.run([str(result.executable)], capture_output=True, timeout=10)
    assert (actual.returncode, actual.stdout, actual.stderr) == (
        baseline.returncode,
        baseline.stdout,
        baseline.stderr,
    ), actual.stderr
