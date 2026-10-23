"""Public orchestration API. Analysis and C emission never execute input programs."""

import ast
import json
import os
import re
import shutil
import sys
import tempfile
import uuid
from dataclasses import dataclass
from hashlib import sha256
from pathlib import Path
from time import perf_counter

from . import __version__
from .backend.generator import generate
from .backend.toolchain import ToolchainError, compile_c, python_config
from .frontend.analysis import analyze
from .frontend.loader import load
from .ir.builder import build as build_ir
from .ir.nodes import Program
from .ir.validator import validate
from .models import Artifact, Diagnostic, Result
from .optimizer import optimize


def _artifact(path: Path, kind: str) -> Artifact:
    return Artifact(kind, str(path.resolve()), sha256(path.read_bytes()).hexdigest())


def _publish_directory(staged: Path, output: Path):
    """Only replace a directory produced by this compiler, retaining rollback."""
    if output.is_symlink():
        raise ValueError("output directory must not be a symlink")
    backup = None
    if output.exists():
        if not output.is_dir():
            raise ValueError("output must be a directory")
        if any(output.iterdir()):
            manifest = output / "manifest.json"
            allowed = {
                "program.c",
                "program.h",
                "calcium_runtime.c",
                "calcium_runtime.h",
                "manifest.json",
                "source-map.json",
                "program.cir.json",
                "program.ast.txt",
            }
            if (
                not manifest.is_file()
                or json.loads(manifest.read_text()).get("generator") != "calcium"
                or any(
                    p.name not in allowed or not p.is_file() or p.is_symlink()
                    for p in output.iterdir()
                )
            ):
                raise ValueError("refusing to replace a directory with non-Calcium contents")
        backup = output.with_name(output.name + ".backup-" + uuid.uuid4().hex)
        output.rename(backup)
    try:
        staged.rename(output)
    except BaseException:
        if backup is not None:
            backup.rename(output)
        raise
    if backup is not None:
        shutil.rmtree(backup)


@dataclass(frozen=True)
class Calcium:
    optimization: int = 0
    target: str = "c"
    compiler: str | None = None
    python: str = sys.executable
    debug: bool = False
    fallback: bool = True
    emit_ir: bool = False
    emit_ast: bool = False
    keep_c: bool = False

    def __post_init__(self):
        if self.optimization not in (0, 1):
            raise ValueError(
                "only -O0 and -O1 are implemented; O2/O3 specialization is not available"
            )
        if self.target not in {"c", "executable", "extension"}:
            raise ValueError(
                "target must be c, executable or extension; a general shared-library ABI is not implemented"
            )
        if self.target == "c" and self.compiler is not None:
            raise ValueError("--compiler applies only to native builds")
        if self.target == "c" and self.keep_c:
            raise ValueError("--keep-c is redundant for target c")

    def _prepare(self, source) -> tuple[Result, object, object, Program | None]:
        result = Result()
        start = perf_counter()
        snapshot = analysis = None
        try:
            config = python_config(str(self.python))
            snapshot = load(source)
            analysis = analyze(snapshot)
            result.metadata = {
                "calcium_version": __version__,
                "python": config,
                "source": str(snapshot.path),
                "source_sha256": snapshot.digest,
                "optimization": self.optimization,
                "target": self.target,
                "native_function_frames": False,
            }
            result.runtime_dependencies = [
                f"CPython {'.'.join(map(str, config['version']))}",
                "matching standard library",
            ]
            result.analysis = {
                "scopes": analysis.scopes,
                "types": analysis.types,
                "ast": ast.dump(analysis.tree, indent=2, include_attributes=True),
            }
            result.diagnostics.extend(analysis.diagnostics)
            result.fallbacks = analysis.fallbacks
            if analysis.fallbacks and not self.fallback:
                for item in analysis.fallbacks:
                    result.diagnostics.append(
                        Diagnostic(
                            "C101", item["reason"], str(snapshot.path), item["line"], item["column"]
                        )
                    )
                return result, snapshot, analysis, None
            if analysis.fallbacks:
                program = Program([], "fallback", snapshot.text)
                result.diagnostics.append(
                    Diagnostic(
                        "C102",
                        "entire module uses CPython code-object fallback; no native coverage",
                        str(snapshot.path),
                        severity="warning",
                    )
                )
            else:
                program = build_ir(analysis.tree)
            validate(program)
            result.optimizations = optimize(program, self.optimization)
            result.ir = program.to_dict()
            result.metadata["mode"] = program.mode
            result.success = True
            return result, snapshot, analysis, program
        except (SyntaxError, UnicodeError) as exc:
            line = getattr(exc, "lineno", 1) or 1
            column = getattr(exc, "offset", 1) or 1
            result.diagnostics.append(Diagnostic("C100", str(exc), str(source), line, column))
        except ToolchainError as exc:
            result.diagnostics.append(Diagnostic("C300", str(exc), stage="environment"))
        except OSError as exc:
            result.diagnostics.append(Diagnostic("C103", str(exc), str(source), stage="loader"))
        finally:
            result.timings["analysis"] = perf_counter() - start
        return result, snapshot, analysis, None

    def analyze(self, source) -> Result:
        return self._prepare(source)[0]

    def inspect(self, source) -> Result:
        return self.analyze(source)

    def transpile(self, source, *, output=None) -> Result:
        if self.target != "c":
            raise ValueError("transpile requires target c; use build for native targets")
        result, snapshot, analysis, program = self._prepare(source)
        if not result.success:
            return result
        output = Path(os.path.abspath(output or Path("build") / snapshot.path.stem))
        if output.is_symlink():
            raise ValueError("output must not be a symlink")
        if snapshot.path.is_relative_to(output.resolve()):
            raise ValueError("output directory must not contain the input source")
        start = perf_counter()
        try:
            output.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix=".calcium-", dir=output.parent) as temporary:
                staged = Path(temporary) / "sources"
                self._emit(
                    staged, result, snapshot, analysis, program, _module_name(snapshot.path.stem)
                )
                _publish_directory(staged, output)
            result.artifacts = [
                _artifact(
                    p, "c" if p.suffix == ".c" else "header" if p.suffix == ".h" else "metadata"
                )
                for p in sorted(output.iterdir())
            ]
        except OSError as exc:
            result.success = False
            result.diagnostics.append(Diagnostic("C301", str(exc), stage="output"))
        result.timings["emission"] = perf_counter() - start
        return result

    def _emit(self, directory, result, snapshot, analysis, program, module_name):
        directory.mkdir(parents=True)
        runtime = Path(__file__).parent / "runtime"
        for name in ("calcium_runtime.h", "calcium_runtime.c"):
            shutil.copyfile(runtime / name, directory / name)
        minor = result.metadata["python"]["version"][1]
        text = generate(
            program,
            filename=str(snapshot.path),
            directory=str(snapshot.path.parent),
            module_name=module_name,
        )
        (directory / "program.c").write_text(text, encoding="utf-8")
        (directory / "program.h").write_text(
            f'#ifndef CALCIUM_PROGRAM_H\n#define CALCIUM_PROGRAM_H\n#define CALCIUM_PY_MINOR {minor}\n#include "calcium_runtime.h"\n#endif\n'
        )
        manifest = {
            "schema_version": 1,
            "generator": "calcium",
            "version": __version__,
            "source_sha256": snapshot.digest,
            "source": str(snapshot.path),
            "mode": program.mode,
            "python_minor": minor,
            "optimization": self.optimization,
            "runtime_abi": 1,
            "fallbacks": result.fallbacks,
            "runtime_dependencies": result.runtime_dependencies,
        }
        (directory / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        )
        mapping = {
            "schema_version": 1,
            "source": str(snapshot.path),
            "functions": {
                fn.symbol: {
                    b.name: [{"register": op.result, "line": op.line} for op in b.instructions]
                    for b in fn.blocks
                }
                for fn in program.functions
            },
        }
        (directory / "source-map.json").write_text(
            json.dumps(mapping, indent=2, sort_keys=True) + "\n"
        )
        if self.emit_ir:
            (directory / "program.cir.json").write_text(
                json.dumps(program.to_dict(), indent=2, sort_keys=True) + "\n"
            )
        if self.emit_ast:
            (directory / "program.ast.txt").write_text(result.analysis["ast"] + "\n")

    def build(self, source, *, output=None) -> Result:
        if self.target == "c":
            return self.transpile(source, output=output)
        result, snapshot, analysis, program = self._prepare(source)
        if not result.success:
            return result
        config = result.metadata["python"]
        suffix = config["config"]["EXT_SUFFIX"] if self.target == "extension" else ""
        output = Path(
            os.path.abspath(
                output or Path("build") / snapshot.path.stem / (snapshot.path.stem + suffix)
            )
        )
        if output.is_symlink():
            raise ValueError("output must not be a symlink")
        if output.resolve() == snapshot.path or output.is_dir() or output.suffix in {".py", ".pyw"}:
            raise ValueError("native output must be a file distinct from the input")
        module_name = _module_name(snapshot.path.stem)
        if self.target == "extension":
            if not output.name.endswith(suffix):
                raise ValueError(f"extension output must end with {suffix}")
            module_name = output.name[: -len(suffix)]
            if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", module_name):
                raise ValueError("extension module name must be an ASCII Python identifier")
        start = perf_counter()
        try:
            output.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix=".calcium-", dir=output.parent) as temporary:
                directory = Path(temporary) / "sources"
                self._emit(directory, result, snapshot, analysis, program, module_name)
                native = Path(temporary) / output.name
                result.metadata["toolchain"] = compile_c(
                    directory,
                    native,
                    config=config,
                    compiler=self.compiler,
                    target=self.target,
                    optimization=self.optimization,
                    debug=self.debug,
                )
                retained = self.keep_c or self.emit_ir or self.emit_ast
                if retained:
                    saved = output.with_name(output.name + ".calcium")
                    _publish_directory(directory, saved)
                    result.artifacts.extend(
                        _artifact(p, "intermediate") for p in sorted(saved.iterdir())
                    )
                os.replace(native, output)
            result.artifacts.append(_artifact(output, self.target))
        except (OSError, ToolchainError) as exc:
            result.success = False
            result.diagnostics.append(Diagnostic("C302", str(exc), stage="toolchain"))
        result.timings["build"] = perf_counter() - start
        return result


def _module_name(name):
    return name if re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name) else "calcium_program"
