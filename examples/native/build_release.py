"""Build, verify and archive standalone demonstration binaries on the CI host."""

import argparse
import json
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

from calcium import Calcium, __version__


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sources = Path(__file__).resolve().parent
    name = f"calcium-{__version__}-native-{sys.platform}-{platform.machine()}"
    with tempfile.TemporaryDirectory(prefix="calcium-examples-") as temporary:
        root = Path(temporary) / name
        root.mkdir()
        for example in [
            "pass",
            "hello",
            "sum_squares",
            "values",
            "coroutines",
            "builtin_tools",
            "demo_package",
        ]:
            source = sources / (example + ".py") if example != "demo_package" else sources / example
            result = Calcium(
                backend="native",
                target="executable",
                optimization=3,
                keep_c=example in {"sum_squares", "demo_package"},
            ).build(source, output=root / example)
            if not result.success:
                raise RuntimeError(result.diagnostics)
            expected = (
                subprocess.check_output([sys.executable, "-m", example], cwd=sources)
                if source.is_dir()
                else subprocess.check_output([sys.executable, str(source)])
            )
            actual = subprocess.check_output([str(root / example)])
            if actual != expected:
                raise RuntimeError(f"{example} output mismatch")
            if "libpython" in subprocess.check_output(["ldd", str(root / example)], text=True):
                raise RuntimeError("unexpected libpython dependency")
        (root / "BUILD.json").write_text(
            json.dumps(
                {
                    "calcium_version": __version__,
                    "backend": "native",
                    "runtime_abi": 5,
                    "platform": platform.platform(),
                    "machine": platform.machine(),
                    "libc": platform.libc_ver(),
                    "compiler": subprocess.check_output(
                        ["gcc", "--version"], text=True
                    ).splitlines()[0],
                    "optimization": 3,
                    "requires": "compatible Linux C/math libraries and ELF loader; no Python installation required",
                },
                indent=2,
            )
            + "\n"
        )
        (root / "README.txt").write_text(
            "Run ./hello or ./sum_squares. The programs need compatible Linux C/math libraries, but no Python interpreter or libpython. BUILD.json identifies the build host/ABI. sum_squares.calcium contains the generated C and CaRT. demo_package demonstrates multi-file imports and containers; demo_package.calcium contains its separate C translation units. coroutines demonstrates native async functions, tasks and locks. builtin_tools demonstrates native lazy iterators, numerical built-ins and selected match patterns. These demonstrate only the documented Bonemarrow 26.10 subset.\n"
        )
        shutil.copyfile(sources.parents[1] / "LICENSE", root / "LICENSE")
        output = args.output / (name + ".tar.gz")
        with tarfile.open(output, "w:gz") as archive:
            archive.add(root, arcname=name)
    print(output)


if __name__ == "__main__":
    main()
