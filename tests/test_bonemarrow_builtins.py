"""Built-ins are exercised through actual generated C against the target Python."""

import os
import random
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from calcium import Calcium

FIXTURES = Path(__file__).parent / "fixtures" / "bonemarrow2610"


def compare(tmp_path, source, level=3, stdin=None):
    file = tmp_path / "program.py"
    file.write_text(source)
    pwork = tmp_path / "python"
    nwork = tmp_path / "native"
    pwork.mkdir()
    nwork.mkdir()
    expected = subprocess.run(
        [sys.executable, str(file)], cwd=pwork, input=stdin, capture_output=True, timeout=30
    )
    result = Calcium(target="executable", optimization=level).build(
        file, output=tmp_path / "program"
    )
    assert result.success, result.diagnostics
    actual = subprocess.run(
        [str(result.executable)],
        cwd=nwork,
        input=stdin,
        capture_output=True,
        timeout=30,
        env={**os.environ, "PATH": "", "PYTHONHOME": "/missing", "PYTHONPATH": "/missing"},
    )
    assert (actual.returncode, actual.stdout) == (expected.returncode, expected.stdout), (
        actual,
        expected,
    )
    assert actual.returncode == 0, actual.stderr
    return result


@pytest.mark.parametrize("level", [0, 3])
@pytest.mark.parametrize("source", sorted(FIXTURES.glob("*.py")), ids=lambda p: p.stem)
def test_builtin_parity(tmp_path, source, level):
    compare(tmp_path, source.read_text(), level)


def test_input_and_exit(tmp_path):
    compare(
        tmp_path,
        "name=input('Name: ')\nprint(repr(name))\ntry: input()\nexcept EOFError: print('eof')\nexit(0)\n",
        stdin="hé😀\r\n".encode(),
    )


def test_large_literals(tmp_path):
    source = (
        "values="
        + repr({str(i): i for i in range(500)})
        + "\nprint(len(values),sum(values.values()))\n"
    )
    source += "values=" + repr(list(range(1000))) + "\nprint(len(values),sum(values))\n"
    compare(tmp_path, source)


def test_numeric_boundaries(tmp_path):
    rng = random.Random(2610)
    values = [rng.uniform(-1e20, 1e20) for _ in range(100)] + [
        2.675,
        1.005,
        1.7976931348623157e308,
        5e-324,
    ]
    pairs = [(value, digits) for value in values for digits in [-308, -25, -2, 0, 2, 20, 308]]
    source = (
        f"pairs={pairs!r}\n"
        + """for value,digits in pairs:
    try: print(round(value,digits))
    except OverflowError: print('overflow')
"""
    )
    compare(tmp_path, source)


@pytest.mark.parametrize("compiler", [n for n in ("gcc", "clang") if shutil.which(n)])
def test_builtin_sanitizers(tmp_path, compiler):
    source = tmp_path / "sanitized.py"
    source.write_text(
        (FIXTURES / "files.py").read_text()
        + (FIXTURES / "iterators.py").read_text()
        + (FIXTURES / "numbers.py").read_text()
        + (FIXTURES / "objects.py").read_text()
        + """\nfor i in range(10000):
    value=list(map(lambda x:x+1,range(8)))
    any(x for x in value)
    cycle=[]
    cycle.append(cycle)
print(len(value))
"""
    )
    work = tmp_path / "python"
    work.mkdir()
    expected = subprocess.run(
        [sys.executable, str(source)], cwd=work, capture_output=True, timeout=30
    )
    emitted = tmp_path / "generated"
    result = Calcium().transpile(source, output=emitted)
    assert result.success, result.diagnostics
    binary = tmp_path / "sanitized"
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-O1",
            "-g",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer",
            "-fno-pie",
            "-no-pie",
            "-pthread",
            "-I",
            str(emitted),
            *map(str, emitted.glob("*.c")),
            "-lm",
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
        timeout=120,
    )
    work = tmp_path / "native"
    work.mkdir()
    run = subprocess.run(
        [str(binary)],
        cwd=work,
        capture_output=True,
        timeout=30,
        env={
            **os.environ,
            "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1",
        },
    )
    assert run.returncode == 0, run.stderr.decode(errors="replace")
    assert run.stderr == b"", run.stderr
    assert run.stdout == expected.stdout, (run, expected)


def test_map_strict_and_protocol_errors(tmp_path):
    source = """class Sequence:
    def __len__(self): return 3
    def __getitem__(self,i): return i*2
print(list(reversed(Sequence())))
class Rounded:
    def __round__(self,n=0): return n+7
print(round(Rounded()),round(Rounded(),2))
"""
    if sys.version_info >= (3, 14):
        source += """print(list(map(lambda a,b:a+b,[1,2],[3,4],strict=True)))
try: list(map(lambda a,b:a+b,[1],[2,3],strict=True))
except ValueError: print('strict length')
"""
    compare(tmp_path, source)


def test_inventory_documents_public_builtins():
    import builtins

    from calcium.backend.native.builtins import inventory

    report = inventory()["builtins"]
    assert {n for n in vars(builtins) if not n.startswith("_")} <= report.keys()
    assert all(item["scope"] for item in report.values())
    assert report["eval"]["status"] == "unsupported"
    assert report["open"]["status"] == "native"
