import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from calcium import Calcium


def test_bundle_relocates_runtime_packages_and_resources(tmp_path):
    project = tmp_path / "source"
    package = project / "app"
    package.mkdir(parents=True)
    (package / "__init__.py").write_text("value = 42\n")
    (package / "data.txt").write_text("bundled data")
    (package / "__main__.py").write_text("""from . import value
from importlib.resources import files
from pathlib import Path
import sys, zlib, sqlite3
print(value, files('app').joinpath('data.txt').read_text())
print(Path(__file__).parent.joinpath('data.txt').read_text())
print(sys.argv[1:], zlib.decompress(zlib.compress(b'ok')), sqlite3.sqlite_version is not None)
print(Path(sys.prefix).name)
""")
    result = Calcium(backend="cpython", optimization=3, fallback=False).bundle(
        package, output=tmp_path / "bundle", dependencies=False
    )
    assert result.success, result.diagnostics
    output = tmp_path / "moved bundle"
    (tmp_path / "bundle").rename(output)
    shutil.rmtree(project)
    environment = dict(
        os.environ,
        PYTHONHOME="/nonexistent",
        PYTHONPATH="/nonexistent",
        PYTHONUSERBASE="/nonexistent",
    )
    run = subprocess.run(
        [output / "run", "two words"],
        cwd=tmp_path,
        env=environment,
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert run.returncode == 0, run.stderr
    assert run.stdout == "42 bundled data\nbundled data\n['two words'] b'ok' True\nruntime\n"
    manifest = json.loads((output / "manifest.json").read_text())
    assert manifest["kind"] == "cpython-bundle"
    assert not list((output / "app").rglob("*.py"))
    assert any("libpython" in p.name for p in (output / "lib").iterdir())
    assert not any(p.is_symlink() for p in output.rglob("*"))
    with pytest.raises(ValueError, match="new directory"):
        Calcium(backend="cpython").bundle(__file__, output=output)


def test_bundle_explicit_resources_and_dependencies(tmp_path):
    # A selected clean venv gives an accurately bounded dependency fixture.
    venv = tmp_path / "venv"
    subprocess.run([sys.executable, "-m", "venv", "--without-pip", venv], check=True)
    python = venv / "bin" / "python"
    config = json.loads(
        subprocess.check_output(
            [python, "-c", "import sysconfig,json; print(json.dumps(sysconfig.get_paths()))"],
            text=True,
        )
    )
    dep = Path(config["purelib"]) / "external_dep"
    dep.mkdir(parents=True)
    (dep / "__init__.py").write_text(
        "from pathlib import Path\nvalue = Path(__file__).with_name('message.txt').read_text()"
    )
    (dep / "message.txt").write_text("external data")
    project = tmp_path / "project"
    project.mkdir()
    (project / "main.py").write_text(
        "from pathlib import Path\nimport external_dep\nprint(external_dep.value, Path(__file__).with_name('data.txt').read_text())"
    )
    (project / "data.txt").write_text("script data")
    result = Calcium(backend="cpython", python=str(python), fallback=False).bundle(
        project / "main.py", output=tmp_path / "bundle", resources=[project / "data.txt"]
    )
    assert result.success, result.diagnostics
    shutil.rmtree(project)
    shutil.rmtree(venv)
    run = subprocess.run([tmp_path / "bundle" / "run"], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, run.stderr
    assert run.stdout == "external data script data\n"
