import json
import subprocess
import sys

import pytest

from calcium import Calcium, __version__


def cli(*args):
    args = list(args)
    if args not in [["--version"], ["--help"]]:
        index = args.index("--") if "--" in args else len(args)
        args[index:index] = ["--cpython"]
    return subprocess.run(
        [sys.executable, "-m", "calcium", *map(str, args)],
        capture_output=True,
        text=True,
        timeout=30,
    )


def test_cli_commands(tmp_path):
    source = tmp_path / "cli.py"
    source.write_text("print(42)")
    assert cli("--version").stdout.startswith(f"Calcium {__version__}")
    info = json.loads(cli("info", "--format", "json").stdout)
    assert info["optimization_levels"] == [0, 1, 2, 3]
    checked = cli("check", source, "--format", "json")
    assert checked.returncode == 0
    assert json.loads(checked.stdout)["metadata"]["mode"] == "native"
    inspection = cli("inspect", source, "--emit-ir", "--format", "json")
    assert json.loads(inspection.stdout)["ir"]["functions"]
    emitted = cli("transpile", source, "--output", tmp_path / "c", "--emit-ir", "--emit-ast")
    assert emitted.returncode == 0, emitted.stderr
    assert (tmp_path / "c" / "program.cir.json").is_file()
    run = cli("run", source, "--output", tmp_path / "program", "-O1")
    assert run.returncode == 0 and run.stdout == "42\n"
    assert cli("check", source, "-O4").returncode == 2
    assert cli("build", source, "--target", "shared").returncode == 2
    assert cli("transpile", source, "--compiler", "gcc").returncode == 2
    assert cli("transpile", source, "--target", "executable").returncode == 2
    assert cli("check", source, "--", "args").returncode == 2


def test_run_arguments_and_exit_status(tmp_path):
    source = tmp_path / "exit.py"
    source.write_text("import sys\nprint(sys.argv[1:])\nraise SystemExit(7)")
    result = cli("run", source, "--output", tmp_path / "program", "--", "--hello", "two words")
    assert result.returncode == 7 and result.stdout == "['--hello', 'two words']\n"
    assert "fallback" not in result.stderr
    strict = cli("check", source, "--no-fallback", "--format", "json")
    assert strict.returncode == 0
    assert json.loads(strict.stdout)["success"]


def test_benchmark(tmp_path):
    source = tmp_path / "bench.py"
    source.write_text("print(42)")
    run = cli("benchmark", source, "--warmups", "0", "--repeat", "2", "--format", "json")
    assert run.returncode == 0, run.stderr
    report = json.loads(run.stdout)
    assert len(report["metadata"]["benchmark"]["calcium_seconds"]) == 2
    assert report["metadata"]["benchmark"]["speedup"] > 0
    assert report["artifacts"] == []
    # Do not publish timings when the two execution routes observably differ.
    source.write_text("print(type(print))\ndef fn():\n    pass\nprint(type(fn))")
    mismatch = cli("benchmark", source, "--warmups", "0", "--repeat", "1", "--format", "json")
    assert mismatch.returncode == 1
    assert "benchmark" not in json.loads(mismatch.stdout)["metadata"]


def test_function_benchmark_cli(tmp_path):
    source = tmp_path / "compute.py"
    source.write_text("def work(n):\n    return n*n\n")
    run = cli(
        source,
        "--benchmark",
        "--function",
        "work",
        "--arguments",
        "[1000]",
        "--repeat",
        "2",
        "-O3",
        "--format",
        "json",
    )
    assert run.returncode == 0, run.stderr
    report = json.loads(run.stdout)["metadata"]["benchmark"]
    assert report["function"] == "work" and len(report["calcium"]["samples_seconds"]) == 2


def test_bundle_run_cli(tmp_path):
    source = tmp_path / "app.py"
    source.write_text("print('bundled')")
    run = cli(source, "--bundle", "--no-dependencies", "--run", "-o", tmp_path / "bundle")
    assert run.returncode == 0 and run.stdout == "bundled\n", run.stderr


def test_preserve_existing_output_on_failure(tmp_path):
    source = tmp_path / "source.py"
    source.write_text("print(1)")
    output = tmp_path / "program"
    assert Calcium(backend="cpython", target="executable").build(source, output=output).success
    previous = output.read_bytes()
    source.write_text("if :")
    assert not Calcium(backend="cpython", target="executable").build(source, output=output).success
    assert output.read_bytes() == previous


def test_reject_symlink_and_foreign_output(tmp_path):
    source = tmp_path / "source.py"
    source.write_text("print(1)")
    target = tmp_path / "target"
    target.mkdir()
    alias = tmp_path / "alias"
    alias.symlink_to(target, target_is_directory=True)
    with pytest.raises(ValueError, match="symlink"):
        Calcium(backend="cpython").transpile(source, output=alias)
    target_file = tmp_path / "mine"
    target_file.write_text("mine")
    alias_file = tmp_path / "alias-file"
    alias_file.symlink_to(target_file)
    with pytest.raises(ValueError, match="symlink"):
        Calcium(backend="cpython", target="executable").build(source, output=alias_file)
    assert target_file.read_text() == "mine"
    emitted = tmp_path / "emitted"
    assert Calcium(backend="cpython").transpile(source, output=emitted).success
    (emitted / "user.txt").write_text("mine")
    with pytest.raises(ValueError, match="non-Calcium"):
        Calcium(backend="cpython").transpile(source, output=emitted)
    assert (emitted / "user.txt").read_text() == "mine"


def test_path_first_build_and_action_options(tmp_path):
    source = tmp_path / "hello.py"
    source.write_text("print(42)")
    output = tmp_path / "hello"
    built = cli(source, "-o", output)
    assert built.returncode == 0, built.stderr
    assert output.is_file()
    assert subprocess.check_output([str(output)]) == b"42\n"
    run = cli(source, "--run", "-o", output)
    assert run.returncode == 0 and run.stdout == "42\n"
    for action in ("--inspect", "--check"):
        report = cli(source, action, "--format", "json")
        assert report.returncode == 0, report.stderr
        assert json.loads(report.stdout)["success"]
    emitted = cli("--transpile", source, "-o", tmp_path / "generated")
    assert emitted.returncode == 0, emitted.stderr
    assert (tmp_path / "generated" / "program.c").is_file()
    assert cli(source, "--inspect", "--run").returncode == 2
    assert "ca SOURCE" in cli("--help").stdout


def test_path_first_package_and_program_arguments(tmp_path):
    package = tmp_path / "my_package"
    package.mkdir()
    (package / "__init__.py").write_text("value = 42")
    (package / "__main__.py").write_text(
        "from . import value\nimport sys\nprint(value, sys.argv[1:])"
    )
    output = tmp_path / "program"
    built = cli(package, "-o", output)
    assert built.returncode == 0, built.stderr
    run = cli(package, "--run", "-o", output, "--", "--inspect", "two words")
    assert run.returncode == 0, run.stderr
    assert run.stdout == "42 ['--inspect', 'two words']\n"
    module = cli("-m", "my_package", "--project-root", tmp_path, "-o", output)
    assert module.returncode == 0, module.stderr
    # Values that happen to be action names are never consumed as selectors.
    report = cli(package, "--check", "--python", "--inspect")
    assert report.returncode == 2


def test_package_build_allows_reported_compatibility_by_default(tmp_path):
    package = tmp_path / "app"
    package.mkdir()
    (package / "__init__.py").write_text("class Item:\n    value = 42\n")
    (package / "__main__.py").write_text("from . import Item\nprint(Item.value)")
    result = cli(package, "--run", "-o", tmp_path / "program")
    assert result.returncode == 0, result.stderr
    assert result.stdout == "42\n"
    assert "definition fallback" in result.stderr
    strict = cli(package, "--check", "--no-fallback", "--format", "json")
    assert strict.returncode == 1
    assert not json.loads(strict.stdout)["success"]
