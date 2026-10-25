"""Bounded native-package benchmarks: three runtimes, checked process observations."""

import argparse
import hashlib
import json
import platform
import random
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

from benchmark import measure, summary

from calcium import Calcium, __version__


def workloads():
    rows = {
        "single_file_startup": ({"main.py": "pass\n"}, "main.py", None),
        "package_startup": ({"pkg/__init__.py": "", "pkg/__main__.py": "pass\n"}, None, "pkg"),
        "one_module_import": (
            {"main.py": "import worker\nprint(worker.VALUE)\n", "worker.py": "VALUE=7\n"},
            "main.py",
            None,
        ),
        "multi_module_import": (
            {
                "main.py": "import a,b,c\nprint(a.x+b.x+c.x)\n",
                "a.py": "x=1\n",
                "b.py": "import a\nx=a.x+1\n",
                "c.py": "import b\nx=b.x+1\n",
            },
            "main.py",
            None,
        ),
        "cross_module_calls": (
            {
                "main.py": "from worker import square\ntotal=0\nfor i in range(100000): total+=square(i)\nprint(total)\n",
                "worker.py": "def square(x): return x*x\n",
            },
            "main.py",
            None,
        ),
        "repeated_imports": (
            {
                "main.py": "for i in range(10000):\n    import worker\nprint(worker.VALUE)\n",
                "worker.py": "VALUE=3\n",
            },
            "main.py",
            None,
        ),
        "circular_imports": (
            {
                "main.py": "import a\nprint(a.x,a.b.y)\n",
                "a.py": "x=4\nimport b\n",
                "b.py": "import a\ny=a.x+1\n",
            },
            "main.py",
            None,
        ),
        "package_initialization": (
            {
                "pkg/__init__.py": "from . import worker\nprint('initialize',worker.VALUE)\n",
                "pkg/worker.py": "VALUE=7\n",
                "pkg/__main__.py": "from .worker import VALUE\nprint(VALUE)\n",
            },
            None,
            "pkg",
        ),
    }
    files = {"main.py": "import graph.m0\nprint(graph.m0.value)\n", "graph/__init__.py": ""}
    for i in range(50):
        files[f"graph/m{i}.py"] = (
            f"from . import m{i + 1}\nvalue=m{i + 1}.value+1\n" if i < 49 else "value=1\n"
        )
    rows["dependency_graph_50"] = (files, "main.py", None)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=9)
    parser.add_argument("--warmups", type=int, default=2)
    args = parser.parse_args()
    if args.repeat < 1 or args.warmups < 0 or sys.platform != "linux":
        parser.error("requires Linux, positive repeat and nonnegative warmups")
    runtime = Path(__file__).resolve().parents[2] / "calcium/runtime/native"
    helper_source = Path(__file__).with_name("measure.c")
    report = {
        "schema_version": 1,
        "calcium_version": __version__,
        "measured_at_utc": datetime.now(timezone.utc).isoformat(),
        "python": sys.version,
        "python_executable": sys.executable,
        "compiler": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
        "platform": platform.platform(),
        "cpu": next(
            line.split(":", 1)[1].strip()
            for line in Path("/proc/cpuinfo").read_text().splitlines()
            if line.startswith("model name")
        ),
        "optimization": 3,
        "repeat": args.repeat,
        "warmups": args.warmups,
        "cart_sha256": hashlib.sha256(
            b"".join(p.read_bytes() for p in sorted(runtime.rglob("*")) if p.is_file())
        ).hexdigest(),
        "helper_sha256": hashlib.sha256(helper_source.read_bytes()).hexdigest(),
        "method": "whole-process wall/child CPU/peak child RSS using minimal C wait4 parent; RSS includes helper inheritance floor; zero RSS samples are null; sources/import graph outside timed working directory; compilation measured separately; shuffled runtime order with seed267; inherited environment, no CPU pinning or background-load control; source-free compiled executables; source bytes retained separately only for Python baseline; main executable size excludes shared libraries",
        "workloads": {},
    }
    rng = random.Random(267)
    with tempfile.TemporaryDirectory(prefix="calcium-package-bench-") as temporary:
        root = Path(temporary)
        helper = root / "measure"
        subprocess.run(
            ["gcc", "-std=c11", "-O2", str(helper_source), "-o", str(helper)], check=True
        )
        for name, (files, entry, module) in workloads().items():
            sources = root / name
            sources.mkdir()
            for relative, text in files.items():
                path = sources / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            row = {
                "files": {p: hashlib.sha256(t.encode()).hexdigest() for p, t in files.items()},
                "runtimes": {},
            }
            commands = {
                "python": [
                    sys.executable,
                    "-B",
                    *(["-m", module] if module else [str(sources / entry)]),
                ]
            }
            for backend in ["cpython", "bmrw"]:
                start = time.perf_counter()
                built = Calcium(
                    backend=backend,
                    target="executable",
                    optimization=3,
                    fallback=True,
                    project_root=sources,
                    cache=False,
                ).build(
                    None if module else sources / entry,
                    module=module,
                    output=root / (name + "-" + backend),
                )
                elapsed = time.perf_counter() - start
                if not built.success:
                    raise RuntimeError(built.diagnostics)
                commands[backend] = [str(built.executable)]
                row["runtimes"][backend] = {
                    "compile_wall_seconds": elapsed,
                    "executable_bytes": built.executable.stat().st_size,
                    "dependencies": subprocess.check_output(
                        ["ldd", built.executable], text=True
                    ).splitlines(),
                }
            row["runtimes"]["python"] = {
                "executable_bytes": Path(sys.executable).resolve().stat().st_size
            }
            samples = {k: [] for k in commands}
            baseline = None
            # Only ordinary CPython can see the source directory. The compiled
            # processes receive no PYTHONPATH/PYTHONHOME and use empty cwd fixtures.
            for iteration in range(args.repeat + args.warmups):
                order = list(commands)
                rng.shuffle(order)
                for backend in order:
                    work = root / f"work-{name}-{iteration}-{backend}"
                    work.mkdir()
                    command = commands[backend]
                    if backend == "python" and module:
                        command = [
                            sys.executable,
                            "-B",
                            "-c",
                            f"import sys,runpy;sys.path.insert(0,{str(sources)!r});runpy.run_module({module!r},run_name='__main__',alter_sys=True)",
                        ]
                    hidden = sources.with_name(sources.name + "-hidden")
                    if backend != "python":
                        sources.rename(hidden)
                    try:
                        observed, metrics = measure(command, work, helper)
                    finally:
                        if backend != "python":
                            hidden.rename(sources)
                    if baseline is None:
                        baseline = observed
                    if observed != baseline or observed[0] != 0:
                        raise RuntimeError(f"correctness mismatch: {name}/{backend}: {observed}")
                    if iteration >= args.warmups:
                        samples[backend].append(metrics)
            for backend, values in samples.items():
                row["runtimes"][backend].update(summary(values))
            report["workloads"][name] = row
            print(
                name,
                {
                    k: round(v["wall_seconds"]["median"] * 1000, 3)
                    for k, v in row["runtimes"].items()
                },
                flush=True,
            )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
