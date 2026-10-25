"""Three-runtime Linux process benchmarks, with correctness checks before timings.

python examples/native/benchmark.py --output docs/benchmarks/native-26.6.json
This executes only the bounded fixtures below; it never launches a GUI app.
"""

import argparse
import hashlib
import json
import platform
import random
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

from calcium import Calcium, __version__

WORKLOADS = {
    "startup": "pass\n",
    "arithmetic": "def work(n):\n    total=0\n    for i in range(n): total+=i*i\n    return total\nprint(work(1000000))\n",
    "recursion": "def fib(n):\n    if n<2: return n\n    return fib(n-1)+fib(n-2)\nprint(fib(25))\n",
    "strings": "value=''\nfor i in range(10000): value+='ab'\nprint(value=='ab'*10000)\n",
    "functions": "def square(x): return x*x\ndef work(n):\n    total=0\n    for i in range(n): total+=square(i)\n    return total\nprint(work(1000000))\n",
    "containers": "values=[i*i for i in range(10000)]\ncounts={i: values[i] for i in range(100)}\nprint(sum(values),len(counts),len(set(values)))\n",
    "objects": "class Item:\n    def __init__(self,value): self.value=value\n    def get(self): return self.value\ntotal=0\nfor i in range(10000): total+=Item(i).get()\nprint(total)\n",
    "imports": 'import json, pathlib, collections\nprint(json.dumps({"ok": True}))\n',
    "filesystem": 'with open("data.txt","w") as stream:\n    stream.write("Calcium"*10000)\nwith open("data.txt") as stream:\n    print(len(stream.read()))\n',
}


def measure(command, directory, helper):
    metrics = directory.parent / (directory.name + ".metrics.json")
    subprocess.run(
        [str(helper), str(directory / "stdout"), str(directory / "stderr"), str(metrics), *command],
        cwd=directory,
        check=True,
        timeout=30,
    )
    measured = json.loads(metrics.read_text())
    status = measured.pop("status")
    # Very short processes can fall below Linux's RSS accounting resolution.
    if measured["peak_rss_bytes"] == 0:
        measured["peak_rss_bytes"] = None
    files = {
        str(p.relative_to(directory)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(directory.rglob("*"))
        if p.is_file() and p.name not in {"stdout", "stderr"}
    }
    observation = [
        status,
        (directory / "stdout").read_bytes(),
        (directory / "stderr").read_bytes(),
        files,
    ]
    return observation, measured


def summary(rows):
    result = {"samples": rows}
    for key in ["wall_seconds", "cpu_seconds", "peak_rss_bytes"]:
        values = [row[key] for row in rows if row[key] is not None]
        if not values:
            result[key] = None
            continue
        quartiles = (
            statistics.quantiles(values, n=4, method="inclusive") if len(values) > 1 else values * 3
        )
        result[key] = {
            "median": statistics.median(values),
            "iqr": quartiles[2] - quartiles[0],
            "min": min(values),
            "max": max(values),
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=9)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument(
        "--application",
        type=Path,
        help="analysis-only compatibility probe; never runs the application",
    )
    args = parser.parse_args()
    if sys.platform != "linux" or args.repeat < 1 or args.warmups < 0:
        parser.error("Linux, positive repeat and nonnegative warmups required")
    report = {
        "schema_version": 1,
        "measured_at_utc": datetime.now(timezone.utc).isoformat(),
        "cart_sha256": hashlib.sha256(
            b"".join(
                p.read_bytes()
                for p in sorted(
                    (Path(__file__).resolve().parents[2] / "calcium" / "runtime" / "native").rglob(
                        "*"
                    )
                )
                if p.is_file()
            )
        ).hexdigest(),
        "calcium_version": __version__,
        "python": sys.version,
        "python_executable": sys.executable,
        "python_prefix": sys.prefix,
        "cpu": next(
            (
                line.split(":", 1)[1].strip()
                for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")
            ),
            platform.machine(),
        ),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "compiler": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
        "optimization": 3,
        "c_flags": ["-std=c11", "-O3", "-fno-fast-math", "-ffp-contract=off"],
        "helper_sha256": hashlib.sha256(
            Path(__file__).with_name("measure.c").read_bytes()
        ).hexdigest(),
        "python_flags": [],
        "environment_controls": "inherited environment; no CPU affinity or background-load control",
        "repeat": args.repeat,
        "warmups": args.warmups,
        "order": "deterministically shuffled per round, seed 266",
        "measurement": "whole-process wall and child CPU time, child peak RSS via a minimal C wait4 helper (Linux KiB converted to bytes); helper startup, fixture setup and validation excluded; RSS has the small helper inheritance floor, zero accounting samples are null",
        "size_scope": "main executable image only; excludes shared libraries and Python standard library",
        "correctness": "stdout, stderr, exit status and private working-directory regular file hashes on every run",
        "limitations": "not CPU-pinned; whole-process samples include startup/shutdown and stdout; GUI responsiveness and runtime hotness are not measured",
        "workloads": {},
    }
    rng = random.Random(266)
    with tempfile.TemporaryDirectory(prefix="calcium-native-bench-") as temporary:
        root = Path(temporary)
        helper = root / "measure"
        subprocess.run(
            [
                "gcc",
                "-std=c11",
                "-O2",
                str(Path(__file__).with_name("measure.c")),
                "-o",
                str(helper),
            ],
            check=True,
        )
        for name, text in WORKLOADS.items():
            source = root / (name + ".py")
            source.write_text(text)
            commands = {"python": [sys.executable, str(source)]}
            row = {"source_sha256": hashlib.sha256(text.encode()).hexdigest(), "runtimes": {}}
            for backend in ["cpython", "native"]:
                compiler = Calcium(
                    backend=backend, target="executable", optimization=3, fallback=True
                )
                begin = time.perf_counter()
                built = compiler.build(source, output=root / (name + "-" + backend))
                build_time = time.perf_counter() - begin
                if built.success:
                    commands[backend] = [str(built.executable)]
                    row["runtimes"][backend] = {
                        "status": "supported",
                        "compile_wall_seconds": build_time,
                        "executable_bytes": built.executable.stat().st_size,
                        "dependencies": built.runtime_dependencies,
                    }
                else:
                    row["runtimes"][backend] = {
                        "status": "unsupported",
                        "diagnostics": [d.message for d in built.diagnostics],
                    }
            row["runtimes"]["python"] = {
                "status": "supported",
                "executable_bytes": Path(sys.executable).resolve().stat().st_size,
            }
            samples = {key: [] for key in commands}
            baseline = None
            for iteration in range(args.repeat + args.warmups):
                order = list(commands)
                rng.shuffle(order)
                for runtime in order:
                    work = root / f"work-{name}-{iteration}-{runtime}"
                    work.mkdir()
                    observed, metrics = measure(commands[runtime], work, helper)
                    if baseline is None:
                        baseline = observed
                    if observed != baseline or observed[0] != 0:
                        raise RuntimeError(
                            f"{name}/{runtime} correctness failed; no result published"
                        )
                    if iteration >= args.warmups:
                        samples[runtime].append(metrics)
            for runtime, rows in samples.items():
                row["runtimes"][runtime].update(summary(rows))
            report["workloads"][name] = row
            print(
                name,
                {
                    r: round(data["wall_seconds"]["median"] * 1000, 3)
                    for r, data in row["runtimes"].items()
                    if data["status"] == "supported"
                },
                flush=True,
            )
        report["real_applications"] = {
            "status": "not executed; native package/import/object support is not implemented"
        }
        if args.application:
            probes = {}
            for backend in ["cpython", "native"]:
                try:
                    result = Calcium(backend=backend, fallback=True, optimization=3).inspect(
                        args.application
                    )
                    probes[backend] = {
                        "success": result.success,
                        "diagnostics": [d.message for d in result.diagnostics],
                        "coverage": result.optimization_report,
                    }
                except ValueError as exc:
                    probes[backend] = {"success": False, "diagnostics": [str(exc)]}
            report["real_applications"]["analysis_only"] = probes
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
