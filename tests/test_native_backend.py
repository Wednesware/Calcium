"""Compile real standalone binaries; compare supported semantics with CPython."""

import json
import os
import random
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from calcium import Calcium

PROGRAMS = [
    "pass\n",
    r"""print("Hello from Calcium!")
print(None, True, False, 0, -1, 1.5, -0.0, sep='|', end='!\n')
print('hé😀' + 'llo', 3*'ab', 'x'*-2)
print(range(3), range(3,0,-1), range, print)
print('flush', flush=True, file=None)
for character in 'a😀é':
    print(character)
""",
    """def sum_squares(n: int) -> int:
    total = 0
    for i in range(n):
        total += i*i
    return total
print(sum_squares(1000000))
""",
    """def fib(n):
    if n < 2:
        return n
    return fib(n-1)+fib(n-2)
print(fib(20))
def identity(n):
    return n
print(identity(True), identity('string'))
""",
    """def sweep(start,stop,step):
    total=0
    for i in range(start,stop,step):
        if i==2: continue
        if i==7: break
        total += i
    else:
        total += 100
    return total
print(sweep(0,10,1), sweep(10,-8,-3), sweep(2,2,1))
x=5
while x:
    x-=1
    if x==3: continue
    print(x)
else:
    print('done')
""",
    """n=0
def advance(value):
    global n
    n+=value
    return n
alias=advance
print(alias(value=3),advance(4),n)
def positional(x, /, y):
    return x-y
print(positional(10,y=3))
""",
    """def arithmetic(a,b):
    return a*b+a-b
print(arithmetic(9223372036854775807,2))
print(arithmetic(-9223372036854775808,-1))
print(arithmetic(999999999999999999999999999999999999999999, -77777777777777777777777777))
x=1
for i in range(100):
    x*=x+1
    if i==8: break
print(x)
for i in range(9223372036854775807, 9223372036854775811):
    print(i)
""",
    """def work(n):
    total=0
    for i in range(n): total+=i
    return total
saved=range
def replacement(n): return saved(3)
range=replacement
print(work(10))
range=saved
def recurse(n):
    if n<2: return n
    return recurse(n-1)+recurse(n-2)
original=recurse
def seven(n): return 7
recurse=seven
print(original(4))
""",
    """def side(value):
    print('side',value)
    return value
print(side(0) and side(3),side(2) or side(4))
print(0 < side(2) < 3, 0 < side(-1) < side(4))
print('a'<'b',True==1,False==0,1==1.0,None==None,None==0)
print(9007199254740993==9007199254740992.0,9007199254740993>9007199254740992.0)
print(1000000000000000000000000000000 < 1e31, -1000000000000000000000000000000 > -1e31)
""",
    """value=''
base='abc'
for i in range(25000):
    value = base + 'def'
    if i == 24999: print(value)
def keep(value):
    if value: return 'alive'
    return None
print(keep(1))
""",
]


@pytest.mark.parametrize("level", [0, 1, 2, 3])
@pytest.mark.parametrize("source_text", PROGRAMS)
def test_native_differential(tmp_path, level, source_text):
    source = tmp_path / "program.py"
    source.write_text(source_text)
    result = Calcium(
        backend="native",
        target="executable",
        optimization=level,
        python="/missing/python",
        keep_c=True,
    ).build(source, output=tmp_path / "program")
    assert result.success, result.diagnostics
    expected = subprocess.run([sys.executable, source], capture_output=True, timeout=15)
    source.unlink()
    env = dict(os.environ, PATH="", PYTHONHOME="/missing", PYTHONPATH="/missing")
    actual = subprocess.run([result.executable], capture_output=True, timeout=15, env=env)
    assert (actual.returncode, actual.stdout, actual.stderr) == (
        expected.returncode,
        expected.stdout,
        expected.stderr,
    )
    assert not result.fallbacks and result.metadata["fallback_allowed"] is False
    retained = tmp_path / "program.calcium"
    emitted = "\n".join(p.read_text() for p in retained.glob("*") if p.suffix in {".c", ".h"})
    assert (
        "Python.h" not in emitted and "PyObject" not in emitted and "Py_Initialize" not in emitted
    )
    assert "libpython" not in subprocess.check_output(["ldd", result.executable], text=True)


def test_random_bigints_and_float_roundtrips(tmp_path):
    rng = random.Random(2616)
    lines = ["def calc(a,b): return a*b+a-b"]
    for i in range(100):
        a = rng.randrange(-(2 ** rng.randrange(1, 600)), 2 ** rng.randrange(1, 600))
        b = rng.randrange(-(2 ** rng.randrange(1, 200)), 2 ** rng.randrange(1, 200))
        lines.append(f"print(calc({a},{b}))")
    for i in range(200):
        f = rng.uniform(-1, 1) * 2.0 ** rng.randrange(-1000, 1000)
        lines.append(f"print({f!r}, {rng.randrange(-(2**100), 2**100)} + {f!r})")
    lines += ["print(5e-324, 1e-4, 1e-5, 1e15, 1e16, 1e309, 1e309-1e309)"]
    source = tmp_path / "random.py"
    source.write_text("\n".join(lines))
    expected = subprocess.run([sys.executable, source], capture_output=True)
    result = Calcium(backend="native", target="executable", optimization=3).build(
        source, output=tmp_path / "random"
    )
    assert result.success, result.diagnostics
    actual = subprocess.run([result.executable], capture_output=True)
    assert (actual.returncode, actual.stdout, actual.stderr) == (
        expected.returncode,
        expected.stdout,
        expected.stderr,
    )


@pytest.mark.parametrize(
    "text,kind",
    [
        ("print(missing)", "NameError"),
        ("def f():\n    print(x)\n    x=1\nf()", "UnboundLocalError"),
        ("print(range(1,2,0))", "ValueError"),
        ('print(1+"s")', "TypeError"),
        ("def f(a): return a\nf()", "TypeError"),
        ("def f(a): return a\nf(1,a=2)", "TypeError"),
        ("def f(): return f()\nf()", "RecursionError"),
        (
            "print(1000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000+0.5)",
            "OverflowError",
        ),
    ],
)
def test_native_runtime_errors(tmp_path, text, kind):
    source = tmp_path / "error.py"
    source.write_text(text)
    result = Calcium(backend="native", target="executable", optimization=3).build(
        source, output=tmp_path / "error"
    )
    assert result.success, result.diagnostics
    actual = subprocess.run([result.executable], capture_output=True, text=True, timeout=10)
    assert actual.returncode == 1 and actual.stderr.startswith(kind + ":"), actual.stderr
    expected = subprocess.run([sys.executable, source], capture_output=True, text=True)
    assert expected.returncode != 0 and kind + ":" in expected.stderr


@pytest.mark.parametrize(
    "text",
    [
        "from __future__ import division",
        "class C(metaclass=type): pass",
        "def f():\n    yield 1",
        'def f(x: print("side")): pass',
        "print(1 << 2)",
        "print(1 is 1)",
        "async def f(): pass",
        "def f():\n    yield 1",
    ],
)
def test_native_unsupported_never_falls_back(tmp_path, text):
    source = tmp_path / "unsupported.py"
    source.write_text(text)
    result = Calcium(backend="native", fallback=True).inspect(source)
    assert not result.success and not result.fallbacks and not result.artifacts
    assert all(d.code == "C160" for d in result.diagnostics if d.severity == "error")
    assert all(
        "no automatic fallback" in d.message for d in result.diagnostics if d.severity == "error"
    )


def test_native_emission_without_python_sdk_and_replacement(tmp_path, monkeypatch):
    import calcium.api

    monkeypatch.setattr(
        calcium.api, "python_config", lambda *a: pytest.fail("native queried Python SDK")
    )
    source = tmp_path / "pass.py"
    source.write_text("pass")
    compiler = Calcium(backend="native", python="/missing/python")
    output = tmp_path / "c"
    for _ in range(2):
        result = compiler.transpile(source, output=output)
        assert result.success, result.diagnostics
    assert json.loads((output / "manifest.json").read_text())["backend"] == "native"
    assert "ca_module" in (output / "program.c").read_text()


@pytest.mark.parametrize("cc", [c for c in ["gcc", "clang"] if shutil.which(c)])
def test_native_sanitizers_and_heap_cycles(tmp_path, cc):
    wrapper = tmp_path / "cc"
    wrapper.write_text(
        f'#!/bin/sh\nexec {shutil.which(cc)} -fsanitize=address,undefined -fno-sanitize-recover=all "$@"\n'
    )
    wrapper.chmod(0o755)
    source = tmp_path / "gc.py"
    source.write_text(
        PROGRAMS[6]
        + "\n"
        + PROGRAMS[9]
        + f"\nprint(range(0,{10**100},7)==range(0,{10**100 + 1},7))\n"
    )
    result = Calcium(
        backend="native", target="executable", compiler=str(wrapper), optimization=3
    ).build(source, output=tmp_path / "gc")
    assert result.success, result.diagnostics
    run = subprocess.run(
        [result.executable],
        capture_output=True,
        text=True,
        env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"),
        timeout=20,
    )
    assert run.returncode == 0, run.stderr
    runtime = Path(__file__).resolve().parents[1] / "calcium" / "runtime" / "native"
    unit = tmp_path / "unit.c"
    unit.write_text("""#include "cart.h"
#include <assert.h>
int main(void){CaRuntime rt={0};CaValue root=CA_UNBOUND;CaFrame frame;ca_push(&rt,&frame,&root,1);
CaIter *obj=ca_allocate(&rt,sizeof(CaIter),CA_ITER_T);root=(CaValue){.tag=CA_ITER_T,.as.obj=(CaObj*)obj};obj->iterable=root;
rt.threshold=1;ca_safepoint(&rt);assert(rt.heap==(CaObj*)obj);
root=CA_UNBOUND;rt.threshold=1;ca_safepoint(&rt);assert(rt.heap==NULL);ca_destroy(&rt);return 0;}
""")
    command = [
        str(wrapper),
        "-std=c11",
        "-I",
        str(runtime / "include"),
        str(unit),
        *(str(p) for p in (runtime / "src").glob("*.c")),
        "-lm",
        "-o",
        str(tmp_path / "unit"),
    ]
    subprocess.run(command, check=True, capture_output=True)
    run = subprocess.run(
        [tmp_path / "unit"],
        capture_output=True,
        text=True,
        env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"),
    )
    assert run.returncode == 0, run.stderr


def test_backend_cli_selection_and_defaults(tmp_path):
    source = tmp_path / "app.py"
    source.write_text("def work(x: int) -> int:\n    return x*x\nprint(work(3))")
    run = subprocess.run(
        [
            sys.executable,
            "-m",
            "calcium",
            str(source),
            "--backend",
            "native",
            "--python",
            "/missing",
            "--run",
            "-O3",
            "-o",
            str(tmp_path / "app"),
        ],
        capture_output=True,
        text=True,
    )
    assert run.returncode == 0 and run.stdout == "9\n", run.stderr
    native = subprocess.run(
        [
            sys.executable,
            "-m",
            "calcium",
            "check",
            str(source),
            "--backend",
            "native",
            "--optimization-report",
            "--format",
            "json",
        ],
        capture_output=True,
        text=True,
    )
    assert native.returncode == 0, native.stderr
    assert json.loads(native.stdout)["metadata"]["backend"] == "native"
    default = subprocess.run(
        [sys.executable, "-m", "calcium", "check", str(source), "--format", "json"],
        capture_output=True,
        text=True,
    )
    assert default.returncode == 0, default.stderr
    assert json.loads(default.stdout)["metadata"]["backend"] == "native"
    info = subprocess.run(
        [
            sys.executable,
            "-m",
            "calcium",
            "--info",
            "--backend",
            "native",
            "--python",
            "/missing",
            "--format",
            "json",
        ],
        capture_output=True,
        text=True,
    )
    assert info.returncode == 0 and not json.loads(info.stdout)["python_runtime_required"]
    with pytest.raises(ValueError, match="extensions"):
        Calcium(backend="native", target="extension")
    with pytest.raises(ValueError, match="standalone"):
        Calcium(backend="native").bundle(source)


def test_failed_native_build_preserves_binary_and_complex_frames(tmp_path):
    source = tmp_path / "app.py"
    body = "\n".join(f"    value{i}=n+{i}" for i in range(100))
    source.write_text(
        "def recurse(n):\n"
        + body
        + "\n    if n<1: return value99\n    return recurse(n-1)\nprint(recurse(100))\n"
    )
    output = tmp_path / "app"
    result = Calcium(backend="native", target="executable", optimization=3).build(
        source, output=output
    )
    assert result.success, result.diagnostics
    actual = subprocess.run([output], capture_output=True, text=True)
    assert actual.returncode == 0 and actual.stdout == "99\n", actual.stderr
    saved = output.read_bytes()
    source.write_text("import ctypes")
    assert not Calcium(backend="native", target="executable").build(source, output=output).success
    assert output.read_bytes() == saved


def test_float_format_ieee_bit_patterns(tmp_path):
    import math
    import struct

    rng = random.Random(266)
    values = [struct.unpack("d", rng.getrandbits(64).to_bytes(8, "little"))[0] for _ in range(1000)]
    literals = [
        x.hex()
        if math.isfinite(x)
        else "NAN"
        if math.isnan(x)
        else "-INFINITY"
        if x < 0
        else "INFINITY"
        for x in values
    ]
    source = tmp_path / "floats.c"
    source.write_text(
        '#include "cart.h"\nint main(void){double values[]={'
        + ",".join(literals)
        + "};for(int i=0;i<1000;i++){ca_print_float(values[i]);putchar(10);}return 0;}"
    )
    runtime = Path(__file__).resolve().parents[1] / "calcium" / "runtime" / "native"
    subprocess.run(
        [
            "gcc",
            "-std=c11",
            "-O2",
            "-I",
            str(runtime / "include"),
            str(source),
            str(runtime / "src" / "floats.c"),
            "-lm",
            "-o",
            str(tmp_path / "floats"),
        ],
        check=True,
    )
    assert subprocess.check_output([tmp_path / "floats"], text=True).splitlines() == [
        str(v) for v in values
    ]


def test_range_equality_exact_large_lengths(tmp_path):
    rng = random.Random(266)
    cases = [
        ((0, 0, 1), (5, 5, -1)),
        ((1, 2, 1), (1, 3, 2)),
        ((0, 10, 3), (0, 12, 3)),
        ((0, 10, 3), (0, 13, 3)),
    ]
    for _ in range(100):
        a = rng.randrange(-(2**200), 2**200)
        step = rng.choice([-7, -2, 1, 3, 5])
        stop = a + step * rng.randrange(0, 2**90)
        cases.append(((a, stop, step), (a, stop + rng.randrange(-3, 4), step)))
    source = tmp_path / "ranges.py"
    source.write_text(
        "\n".join(
            f"print(range({a[0]},{a[1]},{a[2]}) == range({b[0]},{b[1]},{b[2]}))" for a, b in cases
        )
    )
    result = Calcium(backend="native", target="executable", optimization=3).build(
        source, output=tmp_path / "ranges"
    )
    assert result.success, result.diagnostics
    expected = subprocess.check_output([sys.executable, source])
    actual = subprocess.check_output([result.executable])
    assert actual == expected


def test_integer_display_digit_limit(tmp_path):
    source = tmp_path / "large.py"
    source.write_text("x=10\nfor i in range(13): x*=x\nprint(x)\n")
    result = Calcium(backend="native", target="executable", optimization=3).build(
        source, output=tmp_path / "large"
    )
    assert result.success, result.diagnostics
    for command in [[sys.executable, str(source)], [str(result.executable)]]:
        run = subprocess.run(command, capture_output=True, text=True, timeout=15)
        assert run.returncode == 1 and "ValueError:" in run.stderr
        assert not run.stdout
