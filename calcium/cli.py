"""Build a path by default; explicit actions remain available as options/commands."""

import argparse
import json
import os
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

COMMANDS = {"info", "check", "inspect", "transpile", "build", "run", "benchmark"}
ACTIONS = {"--" + name: name for name in COMMANDS}


def normalize_argv(argv):
    """Normalize the path-first interface without touching option values or script args."""
    if not argv or argv == ["--version"] or argv == ["-h"] or argv == ["--help"]:
        return argv
    # Existing subcommands remain an exact compatibility interface. Use ./build
    # to select a source directory whose name is also a command.
    if argv[0] in COMMANDS:
        return argv
    command = "build"
    remaining = []
    selected = []
    takes_value = {
        "--module",
        "-m",
        "--project-root",
        "--include-path",
        "-O",
        "--python",
        "--format",
        "--output",
        "-o",
        "--compiler",
        "--target",
        "--repeat",
        "--warmups",
        "--timeout",
        "--cache-dir",
        "--function",
        "--arguments",
        "--keyword-arguments",
        "--working-directory",
        "--resource",
        "--backend",
        "--use",
    }
    value = False
    for arg in argv:
        if value:
            remaining.append(arg)
            value = False
        elif arg in ACTIONS:
            selected.append(arg)
            command = ACTIONS[arg]
        else:
            remaining.append(arg)
            value = arg in takes_value
    if len(selected) > 1:
        raise ValueError("choose one action: " + ", ".join(selected))
    return [command, *remaining]


def backend_options(command):
    command.add_argument(
        "--backend", choices=["cpython", "native", "bmrw"], action="append", default=[]
    )
    command.add_argument("--use", choices=["cpython", "bmrw"], action="append", default=[])
    command.add_argument("--bmrw", action="store_true")
    command.add_argument("--cpython", action="store_true")


def select_backend(args):
    choices = (
        args.backend
        + args.use
        + (["bmrw"] if args.bmrw else [])
        + (["cpython"] if args.cpython else [])
    )
    choices = {"native" if c == "bmrw" else c for c in choices}
    if len(choices) > 1:
        raise ValueError(
            "conflicting backend selections; choose Bonemarrow (--bmrw) or CPython (--cpython)"
        )
    return next(iter(choices), "native")


def parser():
    root = argparse.ArgumentParser(
        prog="ca",
        usage="ca SOURCE [options] | ca --module MODULE [options]",
        description="Compile a Python script or package to an executable.",
        epilog=(
            "Examples: ca main.py; ca my_package/; ca main.py --run; "
            "ca main.py --inspect; ca main.py --transpile. "
            "Actions: --build (default), --run, --check, --inspect, --transpile, "
            "--benchmark, --info. Existing subcommands also work. "
            "Use ca --build --help for build options."
        ),
    )
    root.add_argument("--version", action="version", version=f"Calcium {__version__}")
    actions = root.add_mutually_exclusive_group()
    for name, help_text in (
        ("build", "build an executable (default)"),
        ("run", "build and execute; pass program arguments after --"),
        ("check", "check supported syntax without building"),
        ("inspect", "show analysis and intermediate representation"),
        ("transpile", "emit C sources without building"),
        ("benchmark", "compare execution with CPython"),
        ("info", "show runtime and compiler availability"),
    ):
        actions.add_argument("--" + name, action="store_true", help=help_text)
    commands = root.add_subparsers(
        prog="ca", dest="command", required=True, title="Legacy commands"
    )
    info = commands.add_parser("info", help="runtime and toolchain availability")
    info.add_argument("--python", default=sys.executable)
    backend_options(info)
    info.add_argument("--format", choices=["text", "json"], default="text")
    for name in ["check", "inspect", "transpile", "build", "run", "benchmark"]:
        cmd = commands.add_parser(name)
        cmd.add_argument("source", type=Path, nargs="?")
        cmd.add_argument("-m", "--module", help="module or package entry, like python -m")
        cmd.add_argument("--project-root", type=Path)
        cmd.add_argument("--include-path", type=Path, action="append", default=[])
        cmd.add_argument(
            "--include-all",
            action="store_true",
            help="compile all Python modules under the project root",
        )
        cmd.add_argument("-O", dest="optimization", type=int, choices=[0, 1, 2, 3], default=0)
        cmd.add_argument("--python", default=sys.executable)
        backend_options(cmd)
        cmd.add_argument("--debug", action="store_true")
        cmd.add_argument("-v", "--verbose", action="store_true")
        cmd.add_argument("--fancy", action="store_true")
        fallback = cmd.add_mutually_exclusive_group()
        fallback.add_argument(
            "--no-fallback",
            dest="fallback",
            action="store_const",
            const=False,
            help="require native C lowering for every compiled source",
        )
        fallback.add_argument(
            "--allow-fallback",
            dest="fallback",
            action="store_const",
            const=True,
            help="explicitly allow reported Python code-object fallback in project modules",
        )
        cmd.set_defaults(fallback=True)
        cmd.add_argument("--report-optimizations", action="store_true")
        cmd.add_argument("--report-fallbacks", action="store_true")
        cmd.add_argument(
            "--optimization-report",
            action="store_true",
            help="show static module/function lowering coverage and fallback reasons",
        )
        if name not in ["run"]:
            cmd.add_argument("--format", choices=["text", "json"], default="text")
        if name in ["inspect", "transpile", "build", "run"]:
            cmd.add_argument("--emit-ir", action="store_true")
            cmd.add_argument("--emit-ast", action="store_true")
        if name in ["transpile", "build", "run"]:
            cmd.add_argument("-o", "--output", type=Path)
        if name in ["build", "run", "benchmark"]:
            cmd.add_argument("--compiler", choices=["gcc", "clang"])
            cmd.add_argument(
                "--no-cache", action="store_true", help="recompile every C translation unit"
            )
            cmd.add_argument(
                "--cache-dir",
                type=Path,
                help="override the native object cache directory",
            )
        if name in ["build", "run"]:
            cmd.add_argument("--keep-c", action="store_true")
            cmd.add_argument(
                "--bundle",
                action="store_true",
                help="include CPython in a relocatable Linux folder",
            )
            cmd.add_argument(
                "--resource",
                action="append",
                default=[],
                type=Path,
                help="additional project data file/directory to bundle",
            )
            cmd.add_argument(
                "--no-dependencies",
                action="store_true",
                help="bundle standard library only, excluding installed packages",
            )
        if name in ["build", "transpile"]:
            cmd.add_argument(
                "--target",
                choices=["c", "executable", "extension", "shared"],
                default="c" if name == "transpile" else "executable",
            )
        if name == "benchmark":
            cmd.add_argument(
                "--function", help="benchmark a function body; exclude startup/imports"
            )
            cmd.add_argument("--arguments", default="[]", help="JSON positional argument array")
            cmd.add_argument(
                "--keyword-arguments", default="{}", help="JSON keyword argument object"
            )
            cmd.add_argument(
                "--working-directory",
                type=Path,
                help="copy this fixture directory for each timed function call",
            )
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
    if args.optimization_report and result.optimization_report:
        from .reporting import format_optimization_report

        report = result.optimization_report
        print(
            json.dumps(report, indent=2)
            if report.get("backend") == "native"
            else format_optimization_report(report),
            file=sys.stderr,
        )
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
            print(f"{args.module or args.source}: OK ({result.metadata['mode']})")
        elif args.command != "run":
            for artifact in result.artifacts:
                print(artifact.path)


def benchmark(compiler, args, program_args):
    if args.repeat < 1 or args.warmups < 0 or args.timeout <= 0:
        raise ValueError("benchmark repeat/timeout must be positive and warmups nonnegative")
    if args.function:
        if compiler.backend == "native":
            raise ValueError(
                "--function benchmarks require the cpython backend; use the native whole-process benchmark suite"
            )
        from .benchmark import benchmark_function

        if args.source is None or args.module is not None:
            raise ValueError("--function currently needs a source file")
        positional, keywords = json.loads(args.arguments), json.loads(args.keyword_arguments)
        if not isinstance(positional, list) or not isinstance(keywords, dict):
            raise ValueError("--arguments must be a JSON array and --keyword-arguments an object")
        return benchmark_function(
            compiler,
            args.source,
            function=args.function,
            arguments=positional,
            keywords=keywords,
            repeat=args.repeat,
            warmups=args.warmups,
            timeout=args.timeout,
            working_directory=args.working_directory,
        )
    print(
        "Benchmark executes the program repeatedly; timings include process startup.",
        file=sys.stderr,
    )
    with tempfile.TemporaryDirectory(prefix="calcium-benchmark-") as tmp:
        result = compiler.build(args.source, module=args.module, output=Path(tmp) / "program")
        if not result.success:
            return result
        executable = result.executable
        result.artifacts.clear()
        if args.module is not None:
            baseline = [args.python, "-m", args.module, *program_args]
            baseline_cwd = None
        elif args.source.is_dir():
            if (args.source / "__init__.py").is_file():
                baseline = [args.python, "-m", args.source.name, *program_args]
                baseline_cwd = None
            else:
                baseline = [args.python, result.metadata["source"], *program_args]
                baseline_cwd = None
        else:
            baseline = [args.python, str(args.source.resolve()), *program_args]
            baseline_cwd = None
        baseline_env = os.environ.copy()
        roots = [str(args.project_root.resolve())] if args.project_root else []
        if args.module is not None and not roots:
            roots.append(str(Path(".").resolve()))
        if (
            args.source is not None
            and args.source.is_dir()
            and (args.source / "__init__.py").is_file()
        ):
            roots.insert(0, str(args.source.resolve().parent))
        roots.extend(str(p.resolve()) for p in args.include_path)
        if roots:
            baseline_env["PYTHONPATH"] = os.pathsep.join(
                [*roots, baseline_env.get("PYTHONPATH", "")]
            )
        commands = [
            baseline,
            [str(executable), *program_args],
        ]
        observations, samples = [], [[], []]
        try:
            for index, command in enumerate(commands):
                run = subprocess.run(
                    command,
                    capture_output=True,
                    timeout=args.timeout,
                    cwd=baseline_cwd if index == 0 else None,
                    env=baseline_env if index == 0 else None,
                )
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
                    run = subprocess.run(
                        commands[index],
                        capture_output=True,
                        timeout=args.timeout,
                        cwd=baseline_cwd if index == 0 else None,
                        env=baseline_env if index == 0 else None,
                    )
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
    try:
        argv = normalize_argv(argv)
    except ValueError as exc:
        root.error(str(exc))
    args = root.parse_args(argv)
    if program_args and args.command not in ["run", "benchmark"]:
        root.error("arguments after -- are only supported by run and benchmark")
    try:
        args.backend = select_backend(args)
        if args.command == "info":
            if args.backend == "native":
                data = {
                    "calcium_version": __version__,
                    "backend": "bmrw",
                    "bonemarrow_version": __version__,
                    "runtime": "CaRT ABI 3",
                    "targets": ["c", "executable"],
                    "backends": {
                        "bmrw": __version__,
                        "cpython": "target interpreter (query with --cpython)",
                    },
                    "python_runtime_required": False,
                    "optimization_levels": [0, 1, 2, 3],
                    "compilers": {n: shutil.which(n) for n in ["gcc", "clang"]},
                }
                print(
                    json.dumps(data, indent=2)
                    if args.format == "json"
                    else f"Calcium {__version__}\nBonemarrow {__version__} (bmrw), CaRT ABI 3\nAvailable backends: bmrw, cpython\nTargets: C11, executable\nNo Python SDK or runtime dependency"
                )
                return 0
            data = {
                "calcium_version": __version__,
                "bonemarrow_version": __version__,
                "backend": "cpython",
                "backends": ["bmrw", "cpython"],
                "python": python_config(args.python),
                "compilers": {name: shutil.which(name) for name in ["gcc", "clang"]},
                "optimization_levels": [0, 1, 2, 3],
                "targets": ["c", "executable", "extension"],
                "native_frames": False,
            }
            print(
                json.dumps(data, indent=2)
                if args.format == "json"
                else f"Calcium {__version__}\nBonemarrow {__version__} (bmrw)\nAvailable backends: bmrw, cpython\nCPython {'.'.join(map(str, data['python']['version']))}\nTargets: C11, executable, CPython extension\nCompilers: {data['compilers']}"
            )
            return 0
        target = getattr(
            args, "target", "executable" if args.command in ["run", "benchmark"] else "c"
        )
        if args.source is None and args.module is None:
            raise ValueError("a source path or --module is required")
        if args.source is not None and args.module is not None:
            raise ValueError("choose a source path or --module, not both")
        if args.command == "transpile" and target != "c":
            raise ValueError("transpile only emits C; use build for native targets")
        if args.command in {"build", "run"}:
            if (args.resource or args.no_dependencies) and not args.bundle:
                raise ValueError("--resource and --no-dependencies require --bundle")
            if args.bundle and (args.keep_c or args.emit_ir or args.emit_ast):
                raise ValueError("use a separate --transpile build to retain bundle intermediates")
        verbose = getattr(args, "verbose", False)

        def progress(message):
            if verbose:
                print("calcium: " + message, file=sys.stderr)

        progress(
            "using Bonemarrow (bmrw)" if args.backend == "native" else "using CPython (cpython)"
        )
        if getattr(args, "fancy", False):
            print(
                "calcium: awakening Bonemarrow..."
                if args.backend == "native"
                else "calcium: awakening CPython...",
                file=sys.stderr,
            )
        compiler = Calcium(
            optimization=args.optimization,
            progress=progress,
            backend=args.backend,
            target=target,
            python=args.python,
            compiler=getattr(args, "compiler", None),
            debug=args.debug,
            fallback=args.fallback,
            project_root=args.project_root,
            include_paths=tuple(args.include_path),
            include_all=args.include_all,
            keep_c=getattr(args, "keep_c", False),
            emit_ir=getattr(args, "emit_ir", False),
            emit_ast=getattr(args, "emit_ast", False),
            cache=not getattr(args, "no_cache", False),
            cache_dir=getattr(args, "cache_dir", None),
        )
        if args.command in ["check", "inspect"]:
            result = compiler.inspect(args.source, module=args.module)
        elif args.command == "transpile":
            result = compiler.transpile(args.source, module=args.module, output=args.output)
        elif args.command == "benchmark":
            result = benchmark(compiler, args, program_args)
        elif getattr(args, "bundle", False):
            if target != "executable":
                raise ValueError("--bundle requires target executable")
            result = compiler.bundle(
                args.source,
                module=args.module,
                output=args.output,
                resources=args.resource,
                dependencies=not args.no_dependencies,
            )
        else:
            result = compiler.build(args.source, module=args.module, output=args.output)
        if result.success and result.executable:
            progress("built " + str(result.executable))
        display(result, args)
        if not result.success:
            return error_status(result)
        if args.command == "run":
            executable = (
                Path(result.metadata["bundle"]["directory"]) / "run"
                if args.bundle
                else result.executable
            )
            status = subprocess.run([str(executable), *program_args]).returncode
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
