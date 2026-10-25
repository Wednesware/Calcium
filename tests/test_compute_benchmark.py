import subprocess

from calcium import Calcium
from calcium.benchmark import benchmark_function


def test_compute_benchmark_validates_results_and_files(tmp_path):
    source = tmp_path / "compute.py"
    source.write_text("""from pathlib import Path

def work(n):
    total = 0
    for i in range(n):
        total += i*i
    return total

def files(n):
    text = Path('input.txt').read_text()
    Path('output.txt').write_text(text * n)
    print(text)
    return [True, n, {'data': text}]
""")
    compiler = Calcium(backend="cpython", optimization=3, fallback=False)
    result = benchmark_function(compiler, source, function="work", arguments=[1000], repeat=3)
    assert result.success, result.diagnostics
    report = result.metadata["benchmark"]
    assert len(report["calcium"]["samples_seconds"]) == 3
    assert report["speedup"] > 0
    assert "startup and imports excluded" in report["measurement"]
    assert report["peak_rss_bytes"]["calcium"] > 0
    assert not result.artifacts
    fixture = tmp_path / "fixture"
    fixture.mkdir()
    (fixture / "input.txt").write_text("hello")
    result = benchmark_function(
        compiler, source, function="files", arguments=[2], repeat=2, working_directory=fixture
    )
    assert result.success, result.diagnostics
    assert not (fixture / "output.txt").exists()


def test_compute_benchmark_refuses_mismatches(tmp_path):
    source = tmp_path / "mismatch.py"
    source.write_text("""from pathlib import Path

def result_type():
    return type(result_type).__name__

def file_effect():
    Path('output').write_text(type(file_effect).__name__)
    return 1
""")
    for name in ["result_type", "file_effect"]:
        result = benchmark_function(
            Calcium(backend="cpython"),
            source,
            function=name,
            repeat=1,
            warmups=0,
        )
        assert not result.success
        assert result.diagnostics[-1].code == "C104"
        assert "benchmark" not in result.metadata


def test_compute_benchmark_timeout_is_a_diagnostic(tmp_path, monkeypatch):
    source = tmp_path / "compute.py"
    source.write_text("def work():\n    return 1\n")
    original = subprocess.run

    def run(*args, **kwargs):
        if kwargs.get("input", "").startswith('{"source":'):
            raise subprocess.TimeoutExpired(args[0], 1)
        return original(*args, **kwargs)

    monkeypatch.setattr(subprocess, "run", run)
    result = benchmark_function(
        Calcium(backend="cpython"),
        source,
        function="work",
        timeout=1,
    )
    assert not result.success
    assert "timed out" in result.diagnostics[-1].message
