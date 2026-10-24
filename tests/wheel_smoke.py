"""Run after installing the wheel; child imports are isolated from the checkout."""

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
        assert version.strip() == "Calcium 26.3", version
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
    print("Installed wheel: isolated CLI, native build, fallback, license, and source-free run OK")


if __name__ == "__main__":
    main()
