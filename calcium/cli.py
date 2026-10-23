"""Noninteractive CLI; executing user programs is confined to run/benchmark."""

import argparse
import json
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path
from time import perf_counter

from . import __version__
from .api import Calcium
from .backend.toolchain import ToolchainError, python_config
from .models import CalciumInternalError


def parser():
    root = argparse.ArgumentParser(
        prog="ca", description="Experimental Python-to-C compiler (embedded CPython)"
    )
    root.add_argument("--version", action="version", version=f"Calcium {__version__}")
    commands = root.add_subparsers(dest="command", required=True)
    info = commands.add_parser("info", help="runtime and toolchain availability")
    info.add_argument("--python", default=sys.executable)
    info.add_argument("--format", choices=["text", "json"], default="text")
    for name in ["check", "inspect", "transpile", "build", "run", "benchmark"]:
        cmd = commands.add_parser(name)
        cmd.add_argument("source", type=Path)
        cmd.add_argument("-O", dest="optimization", type=int, choices=[0, 1, 2, 3], default=0)
        cmd.add_argument("--python", default=sys.executable)
        cmd.add_argument("--debug", action="store_true")
        cmd.add_argument("--verbose", action="store_true")
        cmd.add_argument(
            "--no-fallback",
            action="store_true",
            help="reject code-object fallback (CPython object API remains available)",
        )
        cmd.add_argument("--report-optimizations", action="store_true")
        cmd.add_argument("--report-fallbacks", action="store_true")
        if name not in ["run"]:
            cmd.add_argument("--format", choices=["text", "json"], default="text")
        if name in ["inspect", "transpile", "build", "run"]:
            cmd.add_argument("--emit-ir", action="store_true")
            cmd.add_argument("--emit-ast", action="store_true")
        if name in ["transpile", "build", "run"]:
            cmd.add_argument("--output", type=Path)
        if name in ["build", "run", "benchmark"]:
            cmd.add_argument("--compiler", choices=["gcc", "clang"])
        if name in ["build", "run"]:
            cmd.add_argument("--keep-c", action="store_true")
        if name in ["build", "transpile"]:
            cmd.add_argument(
                "--target",
                choices=["c", "executable", "extension", "shared"],
                default="c" if name == "transpile" else "executable",
            )
        if name == "benchmark":
            cmd.add_argument("--warmups", type=int, default=1)
            cmd.add_argument("--repeat", type=int, default=7)
            cmd.add_argument("--timeout", type=float, default=60)
    return root


def error_status(result):
    return (
        3
        if any(d.code.startswith("C3") for d in result.diagnostics if d.severity == "error")
        else 1
    )


def display(result, args):
    if getattr(args, "format", "text") == "json":
        print(json.dumps(result.to_dict(), indent=2, ensure_ascii=True))
        return
    for diagnostic in result.diagnostics:
        where = (
            f"{diagnostic.path}:{diagnostic.line}:{diagnostic.column}: " if diagnostic.path else ""
        )
        print(
            f"{where}{diagnostic.severity} {diagnostic.code}: {diagnostic.message}", file=sys.stderr
        )
    if args.report_fallbacks:
        print(json.dumps(result.fallbacks, indent=2), file=sys.stderr)
    if args.report_optimizations:
        print(json.dumps(result.optimizations, indent=2), file=sys.stderr)
    if args.verbose and "toolchain" in result.metadata:
        print(json.dumps(result.metadata["toolchain"], indent=2), file=sys.stderr)
    if result.success:
        if args.command == "inspect":
            if args.emit_ast:
                print(result.analysis["ast"])
            print(
                json.dumps(
                    {
                        "analysis": {k: v for k, v in result.analysis.items() if k != "ast"},
                        "ir": result.ir,
                        "fallbacks": result.fallbacks,
                        "optimizations": result.optimizations,
                    },
                    indent=2,
                )
            )
        elif args.command == "check":
            print(f"{args.source}: OK ({result.metadata['mode']})")
        elif args.command != "run":
            for artifact in result.artifacts:
                print(artifact.path)


def benchmark(compiler, args, program_args):
    if args.repeat < 1 or args.warmups < 0 or args.timeout <= 0:
        raise ValueError("benchmark repeat/timeout must be positive and warmups nonnegative")
    print(
        "Benchmark executes the program repeatedly; timings include process startup.",
        file=sys.stderr,
    )
    with tempfile.TemporaryDirectory(prefix="calcium-benchmark-") as tmp:
        result = compiler.build(args.source, output=Path(tmp) / "program")
        if not result.success:
            return result
        executable = result.executable
        result.artifacts.clear()
        commands = [
            [args.python, str(args.source.resolve()), *program_args],
            [str(executable), *program_args],
        ]
        observations, samples = [], [[], []]
        try:
            for command in commands:
                run = subprocess.run(command, capture_output=True, timeout=args.timeout)
                observations.append((run.returncode, run.stdout, run.stderr))
            if observations[0] != observations[1]:
                from .models import Diagnostic

                result.success = False
                result.diagnostics.append(
                    Diagnostic(
                        "C104",
                        "benchmark stdout/stderr/exit-status mismatch; no timings recorded",
                        stage="benchmark",
                    )
                )
                return result
            for iteration in range(args.warmups + args.repeat):
                for index in [0, 1] if iteration % 2 == 0 else [1, 0]:
                    start = perf_counter()
                    run = subprocess.run(commands[index], capture_output=True, timeout=args.timeout)
                    elapsed = perf_counter() - start
                    if (run.returncode, run.stdout, run.stderr) != observations[0]:
                        from .models import Diagnostic

                        result.success = False
                        result.diagnostics.append(
                            Diagnostic(
                                "C104",
                                "benchmark output changed between repetitions; no timings recorded",
                                stage="benchmark",
                            )
                        )
                        return result
                    if iteration >= args.warmups:
                        samples[index].append(elapsed)
        except subprocess.TimeoutExpired as exc:
            raise ValueError(f"benchmark exceeded {args.timeout} second timeout") from exc
        medians = [statistics.median(s) for s in samples]
        result.metadata["benchmark"] = {
            "measurement": "end-to-end subprocess wall time including startup",
            "cpython_seconds": samples[0],
            "calcium_seconds": samples[1],
            "medians": medians,
            "ranges": [[min(s), max(s)] for s in samples],
            "speedup": medians[0] / medians[1],
            "warmups": args.warmups,
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "correctness": "stdout, stderr, exit status; filesystem effects not compared",
        }
        result.artifacts.clear()  # the temporary executable no longer exists
        return result


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    program_args = []
    if "--" in argv:
        split = argv.index("--")
        program_args, argv = argv[split + 1 :], argv[:split]
    root = parser()
    args = root.parse_args(argv)
    if program_args and args.command not in ["run", "benchmark"]:
        root.error("arguments after -- are only supported by run and benchmark")
    try:
        if args.command == "info":
            data = {
                "calcium_version": __version__,
                "python": python_config(args.python),
                "compilers": {name: shutil.which(name) for name in ["gcc", "clang"]},
                "optimization_levels": [0, 1],
                "targets": ["c", "executable", "extension"],
                "native_frames": False,
            }
            print(
                json.dumps(data, indent=2)
                if args.format == "json"
                else f"Calcium {__version__}\nCPython {'.'.join(map(str, data['python']['version']))}\nTargets: C11, executable, CPython extension\nCompilers: {data['compilers']}"
            )
            return 0
        target = getattr(
            args, "target", "executable" if args.command in ["run", "benchmark"] else "c"
        )
        if args.command == "transpile" and target != "c":
            raise ValueError("transpile only emits C; use build for native targets")
        compiler = Calcium(
            optimization=args.optimization,
            target=target,
            python=args.python,
            compiler=getattr(args, "compiler", None),
            debug=args.debug,
            fallback=not args.no_fallback,
            keep_c=getattr(args, "keep_c", False),
            emit_ir=getattr(args, "emit_ir", False),
            emit_ast=getattr(args, "emit_ast", False),
        )
        if args.command in ["check", "inspect"]:
            result = compiler.inspect(args.source)
        elif args.command == "transpile":
            result = compiler.transpile(args.source, output=args.output)
        elif args.command == "benchmark":
            result = benchmark(compiler, args, program_args)
        else:
            result = compiler.build(args.source, output=args.output)
        display(result, args)
        if not result.success:
            return error_status(result)
        if args.command == "run":
            status = subprocess.run([str(result.executable), *program_args]).returncode
            return 128 - status if status < 0 else status
        if args.command == "benchmark" and args.format != "json":
            print(json.dumps(result.metadata["benchmark"], indent=2))
        return 0
    except ValueError as exc:
        print(f"calcium: configuration error: {exc}", file=sys.stderr)
        return 2
    except (ToolchainError, OSError) as exc:
        print(f"calcium: environment error: {exc}", file=sys.stderr)
        return 3
    except CalciumInternalError as exc:
        print(f"calcium: internal compiler error: {exc}", file=sys.stderr)
        return 4


if __name__ == "__main__":
    raise SystemExit(main())
