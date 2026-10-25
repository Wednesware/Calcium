"""Run after installing the wheel; child imports are isolated from the checkout."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    with tempfile.TemporaryDirectory(prefix="calcium-wheel-") as temporary:
        root = Path(temporary)
        package = root / "app"
        package.mkdir()
        (package / "__init__.py").write_text("class Item:\n    value = 42\n")
        (package / "__main__.py").write_text("from . import Item\nprint(Item.value)\n")
        version = subprocess.check_output(
            [sys.executable, "-I", "-m", "calcium", "--version"],
            cwd=root,
            text=True,
        )
        assert version.strip() == "Calcium 26.8", version
        license_name = subprocess.check_output(
            [
                sys.executable,
                "-I",
                "-c",
                "from importlib.metadata import metadata; print(metadata('wednesware-calcium')['License-Expression'])",
            ],
            cwd=root,
            text=True,
        )
        assert license_name.strip() == "MIT", license_name
        run = subprocess.run(
            [
                sys.executable,
                "-I",
                "-m",
                "calcium",
                str(package),
                "--cpython",
                "--run",
                "-O3",
                "--no-cache",
                "-o",
                str(root / "program"),
            ],
            cwd=root,
            capture_output=True,
            text=True,
            timeout=60,
        )
        assert run.returncode == 0, run.stderr
        assert run.stdout == "42\n", run.stdout
        assert "definition fallback" in run.stderr, run.stderr
        # Ensure the installed runtime can execute without original project files.
        package.rename(root / "hidden")
        assert subprocess.check_output([str(root / "program")], cwd=root) == b"42\n"
        native_source = root / "native_app"
        native_source.mkdir()
        (native_source / "__init__.py").write_text("")
        (native_source / "helpers.py").write_text("def square(x: int) -> int:\n    return x*x\n")
        (native_source / "__main__.py").write_text(
            "from .helpers import square\nvalues=[]\nvalues.append(square(9))\nprint(values[0])\n"
        )
        native = root / "standalone"
        run = subprocess.run(
            [
                sys.executable,
                "-I",
                "-m",
                "calcium",
                str(native_source),
                "--backend",
                "native",
                "--python",
                "/missing/python",
                "--run",
                "-O3",
                "-o",
                str(native),
            ],
            cwd=root,
            capture_output=True,
            text=True,
            timeout=60,
        )
        assert run.returncode == 0 and run.stdout == "81\n", run.stderr
        import shutil

        shutil.rmtree(native_source)
        assert (
            subprocess.check_output(
                [str(native)],
                cwd=root,
                env=dict(os.environ, PATH="", PYTHONHOME="/missing", PYTHONPATH="/missing"),
            )
            == b"81\n"
        )
        assert "libpython" not in subprocess.check_output(["ldd", str(native)], text=True)
        library_source = root / "libraries.py"
        library_source.write_text(
            "import json,hashlib,sqlite3\n"
            "from dataclasses import dataclass,asdict\n"
            "@dataclass\nclass Item:\n    name:str\n"
            "db=sqlite3.connect(':memory:')\n"
            "print(json.dumps(asdict(Item('native'))))\n"
            "print(hashlib.sha256(b'abc').hexdigest())\n"
            "print(db.execute('SELECT 42').fetchone())\n"
            "db.close()\n"
        )
        library_binary = root / "native-libraries"
        run = subprocess.run(
            [
                sys.executable,
                "-I",
                "-m",
                "calcium",
                str(library_source),
                "--run",
                "-O3",
                "-o",
                str(library_binary),
            ],
            cwd=root,
            capture_output=True,
            text=True,
            timeout=120,
        )
        assert run.returncode == 0, run.stderr
        assert run.stdout == (
            '{"name": "native"}\n'
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
            "(42,)\n"
        ), run.stdout
        library_source.unlink()
        assert (
            subprocess.check_output(
                [str(library_binary)],
                cwd=root,
                env=dict(os.environ, PATH="", PYTHONHOME="/missing", PYTHONPATH="/missing"),
            ).decode()
            == run.stdout
        )
        assert "libpython" not in subprocess.check_output(["ldd", str(library_binary)], text=True)

    print(
        "Installed wheel: isolated CLI, both backends, fallback, license, and standalone source-free run OK"
    )


if __name__ == "__main__":
    main()
