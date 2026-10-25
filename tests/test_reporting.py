import json
import subprocess
import sys

from calcium import Calcium
from calcium.reporting import format_optimization_report


def test_static_function_counts_and_enclosing_boundaries(tmp_path):
    source = tmp_path / "sample.py"
    source.write_text("""def native(x):
    return x*x

def decorator(fn):
    return fn

@decorator
def annotated(x: int) -> int:
    return x

class C:
    def method(self):
        return 1
    async def later(self):
        pass

def outer(value):
    def inner():
        return value
    return inner

def generator():
    yield 1

π = lambda: 1
""")
    # The module-level lambda blocks the entire module, including otherwise
    # eligible functions; it must never leave them reported as native.
    result = Calcium(backend="cpython", fallback=True, optimization=3).inspect(source)
    report = result.optimization_report
    assert result.success and report["complete"]
    assert report["summary"]["functions_analyzed"] == 9
    assert report["summary"]["functions_native"] == 0
    assert report["primary_fallback_reasons"] == {"lambdas": 9}
    assert report["modules"][0]["functions"][-1]["column"] == 5
    source.write_text(source.read_text().replace("π = lambda: 1", ""))
    result = Calcium(backend="cpython", fallback=True, optimization=3).inspect(source)
    report = result.optimization_report
    assert result.success
    assert report["summary"]["functions_analyzed"] == 8
    assert report["summary"]["functions_native"] == 2
    assert report["summary"]["functions_fallback"] == 6
    assert report["primary_fallback_reasons"] == {
        "classes": 2,
        "closures": 2,
        "decorators": 1,
        "generators": 1,
    }
    assert report["fallback_reason_function_counts"]["annotations"] == 1
    functions = {f["qualname"]: f for f in report["modules"][0]["functions"]}
    assert functions["C.method"]["fallback_boundary"]["name"] == "C"
    assert functions["outer.<locals>.inner"]["fallback_boundary"]["name"] == "outer"
    assert any(d["pass"] == "unboxed-integer-region" for d in report["optimization_decisions"])
    text = format_optimization_report(report)
    assert "Functions analyzed: 8" in text and "Static coverage only" in text
    assert "decorated functions require fallback" in text


def test_future_flags_and_strict_rejection_still_report(tmp_path):
    source = tmp_path / "future.py"
    source.write_text(
        "from __future__ import annotations\ndef good(x):\n    return x\nclass C:\n    def method(self): pass\n"
    )
    for fallback in [True, False]:
        result = Calcium(backend="cpython", fallback=fallback).inspect(source)
        assert result.success == fallback
        report = result.optimization_report
        assert report["complete"]
        assert report["summary"]["functions_analyzed"] == 2
        assert report["summary"]["functions_native"] == 0
        assert report["primary_fallback_reasons"] == {"future_flags": 2}
        assert report["modules"][0]["functions"][1]["fallback_boundary"]["kind"] == "module"


def test_multiple_modules_duplicate_names_and_manifest(tmp_path):
    root = tmp_path / "project"
    root.mkdir()
    (root / "main.py").write_text("import other\ndef same():\n    return 1\n")
    (root / "other.py").write_text(
        "def same():\n    return 1\nclass C:\n    def same(self):\n        pass\n"
    )
    output = tmp_path / "generated"
    result = Calcium(backend="cpython", fallback=True).transpile(root / "main.py", output=output)
    report = result.optimization_report
    assert result.success
    assert report["summary"]["functions_analyzed"] == 3
    assert report["summary"]["functions_native"] == 2
    assert report["summary"]["functions_fallback"] == 1
    assert json.loads((output / "manifest.json").read_text())["optimization_report"] == report


def test_report_never_executes_and_cli_json_remains_clean(tmp_path):
    source = tmp_path / "noexec.py"
    marker = tmp_path / "marker"
    source.write_text(
        f"open({str(marker)!r}, 'w').write('executed')\nclass C:\n    def method(self): pass\n"
    )
    for flags in [[], ["--format", "json"]]:
        run = subprocess.run(
            [
                sys.executable,
                "-m",
                "calcium",
                "check",
                "--cpython",
                str(source),
                "--optimization-report",
                *flags,
            ],
            capture_output=True,
            text=True,
            timeout=20,
        )
        assert run.returncode == 0, run.stderr
        if flags:
            report = json.loads(run.stdout)["optimization_report"]
            assert report["summary"]["functions_fallback"] == 1
            assert "Calcium optimization report" not in run.stderr
        else:
            assert "Calcium optimization report" in run.stderr
            assert "C.method" in run.stderr
        assert not marker.exists()


def test_partial_report_on_later_semantic_syntax_error(tmp_path):
    (tmp_path / "main.py").write_text("import zzz\ndef good():\n    return 1\n")
    (tmp_path / "zzz.py").write_text("return 1\n")
    result = Calcium(backend="cpython", fallback=True).inspect(tmp_path / "main.py")
    assert not result.success
    report = result.optimization_report
    assert not report["complete"]
    assert report["discovered_modules"] == 2
    assert report["summary"]["modules_analyzed"] == 1
    assert report["summary"]["functions_native"] == 1
    assert "Partial report" in format_optimization_report(report)


def test_namespace_and_zero_function_module_are_not_fallback_functions(tmp_path):
    (tmp_path / "main.py").write_text("import ns.worker\n")
    (tmp_path / "ns").mkdir()
    (tmp_path / "ns" / "worker.py").write_text("class Empty: pass\n")
    result = Calcium(backend="cpython", fallback=True).inspect(tmp_path / "main.py")
    assert result.success
    report = result.optimization_report
    assert report["complete"] and report["summary"]["modules_analyzed"] == 3
    assert report["summary"]["module_modes"]["namespace"] == 1
    assert report["summary"]["functions_analyzed"] == 0
    assert report["summary"]["classes_analyzed"] == 1
    assert report["primary_fallback_reasons"] == {}
    assert any(r["category"] == "classes" for m in report["modules"] for r in m["fallbacks"])
