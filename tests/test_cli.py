import json
import subprocess
import sys

import pytest

from calcium import Calcium


def cli(*args):
    return subprocess.run(
        [sys.executable, "-m", "calcium", *map(str, args)],
        capture_output=True,
        text=True,
        timeout=30,
    )


def test_cli_commands(tmp_path):
    source = tmp_path / "cli.py"
    source.write_text("print(42)")
    assert cli("--version").stdout.startswith("Calcium 0.1.0")
    info = json.loads(cli("info", "--format", "json").stdout)
    assert info["optimization_levels"] == [0, 1]
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
    assert cli("check", source, "-O3").returncode == 2
    assert cli("build", source, "--target", "shared").returncode == 2
    assert cli("transpile", source, "--compiler", "gcc").returncode == 2
    assert cli("transpile", source, "--target", "executable").returncode == 2
    assert cli("check", source, "--", "args").returncode == 2


def test_run_arguments_and_exit_status(tmp_path):
    source = tmp_path / "exit.py"
    source.write_text("import sys\nprint(sys.argv[1:])\nraise SystemExit(7)")
    result = cli("run", source, "--output", tmp_path / "program", "--", "--hello", "two words")
    assert result.returncode == 7 and result.stdout == "['--hello', 'two words']\n"
    assert "fallback" in result.stderr
    strict = cli("check", source, "--no-fallback", "--format", "json")
    assert strict.returncode == 1
    assert not json.loads(strict.stdout)["success"]


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


def test_preserve_existing_output_on_failure(tmp_path):
    source = tmp_path / "source.py"
    source.write_text("print(1)")
    output = tmp_path / "program"
    assert Calcium(target="executable").build(source, output=output).success
    previous = output.read_bytes()
    source.write_text("if :")
    assert not Calcium(target="executable").build(source, output=output).success
    assert output.read_bytes() == previous


def test_reject_symlink_and_foreign_output(tmp_path):
    source = tmp_path / "source.py"
    source.write_text("print(1)")
    target = tmp_path / "target"
    target.mkdir()
    alias = tmp_path / "alias"
    alias.symlink_to(target, target_is_directory=True)
    with pytest.raises(ValueError, match="symlink"):
        Calcium().transpile(source, output=alias)
    target_file = tmp_path / "mine"
    target_file.write_text("mine")
    alias_file = tmp_path / "alias-file"
    alias_file.symlink_to(target_file)
    with pytest.raises(ValueError, match="symlink"):
        Calcium(target="executable").build(source, output=alias_file)
    assert target_file.read_text() == "mine"
    emitted = tmp_path / "emitted"
    assert Calcium().transpile(source, output=emitted).success
    (emitted / "user.txt").write_text("mine")
    with pytest.raises(ValueError, match="non-Calcium"):
        Calcium().transpile(source, output=emitted)
    assert (emitted / "user.txt").read_text() == "mine"
