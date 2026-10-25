"""Opt-in UBSan checks; CI enables these with CALCIUM_SANITIZERS=1."""

import os
import shutil
import subprocess
import sys

import pytest

from calcium import Calcium

pytestmark = pytest.mark.skipif(
    os.environ.get("CALCIUM_SANITIZERS") != "1", reason="set CALCIUM_SANITIZERS=1 for UBSan"
)


@pytest.mark.parametrize("compiler", [name for name in ("gcc", "clang") if shutil.which(name)])
def test_ubsan_checked_arithmetic_and_error_cleanup(tmp_path, compiler):
    wrapper = tmp_path / "sanitized-compiler"
    wrapper.write_text(
        f'#!/bin/sh\nexec {compiler} -fsanitize=undefined -fno-sanitize-recover=all "$@"\n'
    )
    wrapper.chmod(0o755)
    source = tmp_path / "sanitized.py"
    source.write_text("""def scalar(a, b):
    return a * b + a - b
def arithmetic(a, b):
    return a + b, a - b, a * b
for a in [-2**100, -2**63, -1, 0, 1, 2**63-1, 2**100]:
    for b in [-2**100, -2**63, -1, 0, 1, 2**63-1, 2**100]:
        print(arithmetic(a, b), scalar(a, b))
def fail(value):
    left, *middle, right = value
    assert left < right, (left, right)
    return f'{left}:{middle!r}:{right}'
def exercise():
    for value in [[], [1], [3, 2, 1], [1, 2, 3, 4]]:
        try:
            print(fail(value))
        except (ValueError, AssertionError) as error:
            print(type(error).__name__, str(error))
exercise()
""")
    result = Calcium(
        backend="cpython",
        target="executable",
        optimization=3,
        fallback=True,
        compiler=str(wrapper),
        cache=False,
    ).build(source)
    assert result.success, result.diagnostics
    expected = subprocess.run([sys.executable, str(source)], capture_output=True, timeout=15)
    actual = subprocess.run([str(result.executable)], capture_output=True, timeout=15)
    assert (actual.returncode, actual.stdout, actual.stderr) == (
        expected.returncode,
        expected.stdout,
        expected.stderr,
    )
