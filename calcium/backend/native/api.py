"""Native orchestration deliberately never queries a target Python SDK."""

import ast
import json
import os
import platform
import shutil
import subprocess
import tempfile
from pathlib import Path
from time import perf_counter

from ... import __version__
from ...frontend.loader import load
from ...ir.builder import build as build_ir
from ...models import Diagnostic, Result
from ...optimizer import optimize
from .frontend import prepare
from .generator import generate


def analyze(compiler, source, module):
    result = Result(
        metadata={
            "calcium_version": __version__,
            "backend": "native",
            "mode": "native",
            "target": compiler.target,
            "optimization": compiler.optimization,
            "fallback_allowed": False,
        }
    )
    start = perf_counter()
    try:
        if module is not None or source is None or not Path(source).is_file():
            raise ValueError(
                "native backend 26.6 requires a single Python file; modules/packages are not implemented"
            )
        snapshot = load(source)
        original, tree, errors = prepare(snapshot)
        result.diagnostics.extend(errors)
        result.metadata.update(source=str(snapshot.path), source_sha256=snapshot.digest)
        report = {
            "schema_version": 1,
            "backend": "native",
            "measurement": "static lowering decisions, not runtime counts",
            "unsupported_features": [d.message for d in errors if d.severity == "error"],
        }
        result.optimization_report = report
        if any(d.severity == "error" for d in errors):
            return result, None
        program = build_ir(tree)
        decisions = optimize(program, compiler.optimization)
        # These pass records are shared CIR transformations, with CaRT rather
        # than CPython as the slow path in this backend.
        for decision in decisions:
            if "assumption" in decision:
                decision["assumption"] = decision["assumption"].replace(
                    "overflow/subclasses use CPython",
                    "overflow uses CaRT arbitrary precision; Python subclasses are unsupported",
                )
            if decision["pass"] == "unboxed-integer-region":
                decision["boxing"] = "tagged primitive at public return; no numeric heap box"
        result.optimizations = decisions
        text, coverage = generate(program, compiler.optimization)
        report.update(coverage)
        report["optimization_decisions"] = decisions
        result.ir = program.to_dict()
        result.analysis = {"ast": ast.dump(original, indent=2, include_attributes=True)}
        result.runtime_dependencies = ["C11 platform C library", "libm"]
        result.success = True
        return result, (snapshot, text)
    except (SyntaxError, UnicodeError) as exc:
        result.diagnostics.append(
            Diagnostic(
                "C100",
                str(exc),
                str(source),
                getattr(exc, "lineno", 1) or 1,
                getattr(exc, "offset", 1) or 1,
            )
        )
    except OSError as exc:
        result.diagnostics.append(Diagnostic("C103", str(exc), str(source)))
    finally:
        result.timings["analysis"] = perf_counter() - start
    return result, None


def emit(directory, result, plan):
    snapshot, text = plan
    runtime = Path(__file__).resolve().parents[2] / "runtime" / "native"
    directory.mkdir()
    (directory / "program.c").write_text(text)
    shutil.copyfile(runtime / "include" / "cart.h", directory / "calcium_runtime.h")
    # Use the established owned-output publication mechanism and flat artifacts.
    (directory / "cart.h").write_text('#include "calcium_runtime.h"\n')
    for path in sorted((runtime / "src").glob("*.c")):
        shutil.copyfile(path, directory / ("cart_" + path.name))
    manifest = {
        "generator": "calcium",
        "version": __version__,
        "backend": "native",
        "runtime_abi": 1,
        "source": str(snapshot.path),
        "source_sha256": snapshot.digest,
        "optimization_report": result.optimization_report,
        "modules": [{"c_file": p.name} for p in directory.glob("*.c") if p.name != "program.c"],
        "native_files": ["cart.h"],
    }
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def build(compiler, source, module, output, *, transpile=False):
    from ...api import _artifact, _publish_directory

    result, plan = analyze(compiler, source, module)
    if not result.success:
        return result
    snapshot, _ = plan
    if compiler.target == "extension":
        raise ValueError("native backend cannot produce CPython extensions")
    output = Path(
        os.path.abspath(
            output
            or Path("build")
            / snapshot.path.stem
            / (snapshot.path.stem + ("-c" if transpile else ""))
        )
    )
    if output.is_symlink():
        raise ValueError("output must not be a symlink")
    if transpile and snapshot.path.is_relative_to(output):
        raise ValueError("output directory must not contain the input source")
    if not transpile and (
        output == snapshot.path or output.is_dir() or output.suffix in {".py", ".pyw"}
    ):
        raise ValueError("native output must be a file distinct from input")
    start = perf_counter()
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".calcium-native-", dir=output.parent) as temporary:
            staged = Path(temporary) / "sources"
            emit(staged, result, plan)
            if compiler.emit_ir:
                (staged / "program.cir.json").write_text(json.dumps(result.ir, indent=2) + "\n")
            if compiler.emit_ast:
                (staged / "program.ast.txt").write_text(result.analysis["ast"] + "\n")
            if transpile:
                _publish_directory(staged, output)
                result.artifacts = [
                    _artifact(
                        p, "c" if p.suffix == ".c" else "header" if p.suffix == ".h" else "metadata"
                    )
                    for p in sorted(output.iterdir())
                ]
            else:
                if platform.system() != "Linux":
                    raise ValueError(
                        "native executable builds are initially validated on Linux only"
                    )
                cc = shutil.which(compiler.compiler or "gcc") or (
                    shutil.which("clang") if compiler.compiler is None else None
                )
                if not cc:
                    raise OSError("C compiler not found")
                binary = Path(temporary) / "program"
                command = [
                    cc,
                    "-std=c11",
                    f"-O{compiler.optimization}",
                    "-fno-fast-math",
                    "-ffp-contract=off",
                    "-I",
                    str(staged),
                    *(str(p) for p in sorted(staged.glob("*.c"))),
                    "-o",
                    str(binary),
                    "-lm",
                ]
                if compiler.debug:
                    command[1:1] = ["-g", "-Wall", "-Wextra"]
                run = subprocess.run(command, capture_output=True, text=True, timeout=120)
                result.metadata["toolchain"] = {
                    "command": command,
                    "warnings": run.stderr,
                    "compiler": subprocess.check_output([cc, "--version"], text=True).splitlines()[
                        0
                    ],
                }
                if run.returncode:
                    raise OSError(f"C compiler failed: {run.stderr}")
                if compiler.keep_c or compiler.emit_ir or compiler.emit_ast:
                    saved = output.with_name(output.name + ".calcium")
                    _publish_directory(staged, saved)
                    result.artifacts.extend(
                        _artifact(p, "intermediate") for p in sorted(saved.iterdir())
                    )
                os.replace(binary, output)
                result.artifacts.append(_artifact(output, "executable"))
    except (OSError, subprocess.SubprocessError) as exc:
        result.success = False
        result.diagnostics.append(Diagnostic("C302", str(exc), stage="toolchain"))
    result.timings["emission" if transpile else "build"] = perf_counter() - start
    return result
