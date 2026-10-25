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
from .backend.project import launcher, module_id
from .backend.toolchain import ToolchainError, compile_c, python_config
from .frontend.analysis import analyze
from .frontend.project import discover
from .ir.builder import build as build_ir
from .ir.nodes import Program
from .ir.validator import validate
from .models import Artifact, Diagnostic, Result
from .optimizer import optimize
from .reporting import module_report, optimization_report


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
            if manifest.is_file():
                previous = json.loads(manifest.read_text())
                if previous.get("backend") == "native":
                    allowed.add("cart.h")
                for record in previous.get("modules", []):
                    for key in ("c_file", "ir_file", "ast_file"):
                        name = record.get(key)
                        if name and Path(name).name == name:
                            allowed.add(name)
            allowed.add("calcium_modules.c")
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
    fallback: bool | None = None
    emit_ir: bool = False
    emit_ast: bool = False
    keep_c: bool = False
    project_root: str | Path | None = None
    include_paths: tuple[str | Path, ...] = ()
    include_all: bool = False
    cache: bool = True
    cache_dir: str | Path | None = None
    backend: str = "cpython"

    def __post_init__(self):
        if self.backend not in {"cpython", "native"}:
            raise ValueError("backend must be cpython or native")
        if self.backend == "native" and self.target == "extension":
            raise ValueError("native backend cannot produce CPython extensions")
        if self.optimization not in (0, 1, 2, 3):
            raise ValueError("optimization must be between 0 and 3")
        if self.target not in {"c", "executable", "extension"}:
            raise ValueError(
                "target must be c, executable or extension; a general shared-library ABI is not implemented"
            )
        if self.target == "c" and self.compiler is not None:
            raise ValueError("--compiler applies only to native builds")
        if self.target == "c" and self.keep_c:
            raise ValueError("--keep-c is redundant for target c")

    def _prepare(self, source=None, *, module=None):
        result = Result()
        start = perf_counter()
        snapshot = analysis = None
        coverage = []
        discovered_modules = None
        active = str(source or module)
        try:
            config = python_config(str(self.python))
            project = discover(
                source,
                module=module,
                root=self.project_root,
                include_paths=self.include_paths,
                include_all=self.include_all,
                library=self.target == "extension",
            )
            discovered_modules = len(project.modules)
            snapshot = project.entry.source
            if snapshot is None:
                raise ValueError("entry must contain executable Python source")
            if self.target == "executable" and project.entry.package:
                raise ValueError(
                    "package needs __main__.py to build an executable; use --module package.entry or --target extension"
                )
            project_build = (
                project.directory_input or project.module_entry or len(project.modules) > 1
            )
            allow_fallback = self.fallback if self.fallback is not None else not project_build
            result.metadata = {
                "calcium_version": __version__,
                "backend": "cpython",
                "python": config,
                "source": str(snapshot.path),
                "source_sha256": snapshot.digest,
                "optimization": self.optimization,
                "target": self.target,
                "native_function_frames": False,
                "project_root": str(project.roots[0]),
                "project_roots": [str(p) for p in project.roots],
                "entry_module": project.entry.name,
                "module_entry": project.module_entry,
                "fallback_allowed": allow_fallback,
            }
            result.runtime_dependencies = [
                f"CPython {'.'.join(map(str, config['version']))}",
                "matching standard library",
            ]
            result.runtime_dependencies.extend(
                f"external import: {name}" for name in sorted(project.external)
            )
            analyses, programs, records = {}, {}, []
            for name, item in sorted(project.modules.items()):
                if item.namespace:
                    coverage.append(
                        {
                            "name": name,
                            "path": str(item.path),
                            "mode": "namespace",
                            "fallbacks": [],
                            "functions": [],
                            "classes": [],
                        }
                    )
                    records.append(
                        {
                            "name": name,
                            "source": str(item.path),
                            "directory": str(item.directory),
                            "package": True,
                            "namespace": True,
                            "search_locations": [str(p) for p in item.namespace_paths],
                            "mode": "namespace",
                            "executor": None,
                            "c_file": None,
                            "source_sha256": None,
                        }
                    )
                    continue
                active = str(item.path)
                current = analyze(item.source)
                analyses[name] = current
                coverage.append(module_report(name, item.source, current))
                result.diagnostics.extend(current.diagnostics)
                reasons = [
                    dict(reason, module=name, path=str(item.path)) for reason in current.fallbacks
                ]
                result.fallbacks.extend(reasons)
                if reasons and not allow_fallback:
                    for reason in reasons:
                        result.diagnostics.append(
                            Diagnostic(
                                "C101",
                                reason["reason"],
                                str(item.path),
                                reason["line"],
                                reason["column"],
                            )
                        )
                    continue
                if reasons:
                    whole_module = any(reason["boundary"] != "definition" for reason in reasons)
                    if whole_module:
                        program = Program([], "fallback", item.source.text)
                    else:
                        program = build_ir(
                            current.tree,
                            definition_fallbacks=current.definition_fallbacks,
                            source=item.source.text,
                        )
                    result.diagnostics.append(
                        Diagnostic(
                            "C102",
                            f"module {name} uses CPython {'whole-module' if whole_module else 'definition'} fallback",
                            str(item.path),
                            severity="warning",
                        )
                    )
                else:
                    program = build_ir(current.tree)
                validate(program)
                result.optimizations.extend(
                    dict(report, module=name) for report in optimize(program, self.optimization)
                )
                programs[name] = program
                identity = module_id(name)
                records.append(
                    {
                        "name": name,
                        "source": str(item.path),
                        "directory": str(item.directory),
                        "package": item.package,
                        "namespace": False,
                        "search_locations": [str(item.directory)] if item.package else [],
                        "mode": program.mode,
                        "executor": "ca_exec_" + identity,
                        "c_file": "module_" + identity + ".c",
                        "source_sha256": item.source.digest,
                    }
                )
            for item in project.dynamic:
                result.diagnostics.append(
                    Diagnostic(
                        "C112",
                        item["reason"],
                        str(project.modules[item["module"]].path),
                        item["line"],
                        severity="warning",
                    )
                )
            analysis = analyses.get(project.entry.name)
            if any(d.severity == "error" for d in result.diagnostics):
                return result, snapshot, analysis, None
            entry_program = programs[project.entry.name]
            result.ir = entry_program.to_dict()
            result.ir["modules"] = {name: program.to_dict() for name, program in programs.items()}
            result.analysis = {
                "scopes": analysis.scopes,
                "types": analysis.types,
                "ast": ast.dump(analysis.tree, indent=2, include_attributes=True),
                "modules": {
                    name: {"scopes": a.scopes, "types": a.types} for name, a in analyses.items()
                },
                "imports": project.imports,
                "external_imports": sorted(project.external),
                "dynamic_imports": project.dynamic,
            }
            modes = {p.mode for p in programs.values()}
            result.metadata["mode"] = next(iter(modes)) if len(modes) == 1 else "mixed"
            result.metadata["modules"] = records
            result.metadata["project_sha256"] = sha256(
                json.dumps(
                    {r["name"]: r["source_sha256"] for r in records}, sort_keys=True
                ).encode()
            ).hexdigest()
            result.success = True
            return result, snapshot, analysis, (project, analyses, programs, records)
        except (SyntaxError, UnicodeError) as exc:
            line, column = getattr(exc, "lineno", 1) or 1, getattr(exc, "offset", 1) or 1
            result.diagnostics.append(
                Diagnostic("C100", str(exc), getattr(exc, "filename", None) or active, line, column)
            )
        except ToolchainError as exc:
            result.diagnostics.append(Diagnostic("C300", str(exc), stage="environment"))
        except OSError as exc:
            result.diagnostics.append(Diagnostic("C103", str(exc), active, stage="loader"))
        finally:
            if discovered_modules is not None:
                result.optimization_report = optimization_report(
                    coverage,
                    result.optimizations,
                    complete=len(coverage) == discovered_modules,
                    discovered_modules=discovered_modules,
                )
            result.timings["analysis"] = perf_counter() - start
        return result, snapshot, analysis, None

    def analyze(self, source=None, *, module=None) -> Result:
        if self.backend == "native":
            from .backend.native.api import analyze

            return analyze(self, source, module)[0]
        return self._prepare(source, module=module)[0]

    def inspect(self, source=None, *, module=None) -> Result:
        return self.analyze(source, module=module)

    def transpile(self, source=None, *, module=None, output=None) -> Result:
        if self.target != "c":
            raise ValueError("transpile requires target c; use build for native targets")
        if self.backend == "native":
            from .backend.native.api import build

            return build(self, source, module, output, transpile=True)
        result, snapshot, analysis, program = self._prepare(source, module=module)
        if not result.success:
            return result
        output = Path(os.path.abspath(output or Path("build") / _entry_stem(program)))
        if output.is_symlink():
            raise ValueError("output must not be a symlink")
        if any(item.path.is_relative_to(output.resolve()) for item in program[0].modules.values()):
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

    def _emit(self, directory, result, snapshot, analysis, plan, module_name):
        project, analyses, programs, original_records = plan
        records = [dict(record) for record in original_records]
        directory.mkdir(parents=True)
        runtime = Path(__file__).parent / "runtime"
        for name in ("calcium_runtime.h", "calcium_runtime.c", "calcium_modules.c"):
            shutil.copyfile(runtime / name, directory / name)
        minor = result.metadata["python"]["version"][1]
        for record in records:
            if record["namespace"]:
                continue
            name = record["name"]
            text = generate(
                programs[name],
                filename=record["source"],
                directory=record["directory"],
                module_name=module_name,
                python_executable=result.metadata["python"]["executable"],
                executor=record["executor"],
            )
            (directory / record["c_file"]).write_text(text, encoding="utf-8")
            if self.emit_ir:
                record["ir_file"] = record["c_file"] + ".cir.json"
                (directory / record["ir_file"]).write_text(
                    json.dumps(programs[name].to_dict(), indent=2, sort_keys=True) + "\n"
                )
            if self.emit_ast:
                record["ast_file"] = record["c_file"] + ".ast.txt"
                (directory / record["ast_file"]).write_text(
                    ast.dump(analyses[name].tree, indent=2, include_attributes=True) + "\n"
                )
        text = launcher(
            records,
            project.entry.name,
            module_entry=project.module_entry,
            directory=str(project.roots[0] if project.module_entry else snapshot.path.parent),
            python_executable=result.metadata["python"]["executable"],
            extension_name=module_name,
            roots=project.roots,
        )
        (directory / "program.c").write_text(text, encoding="utf-8")
        (directory / "program.h").write_text(
            f'#ifndef CALCIUM_PROGRAM_H\n#define CALCIUM_PROGRAM_H\n#define CALCIUM_PY_MINOR {minor}\n#include "calcium_runtime.h"\n#endif\n'
        )
        manifest = {
            "schema_version": 2,
            "generator": "calcium",
            "backend": "cpython",
            "version": __version__,
            "source_sha256": snapshot.digest,
            "source": str(snapshot.path),
            "project_sha256": result.metadata["project_sha256"],
            "entry_module": project.entry.name,
            "module_entry": project.module_entry,
            "mode": result.metadata["mode"],
            "modules": records,
            "project_roots": [str(root) for root in project.roots],
            "imports": project.imports,
            "external_imports": sorted(project.external),
            "dynamic_imports": project.dynamic,
            "python_minor": minor,
            "python_executable": result.metadata["python"]["executable"],
            "optimization": self.optimization,
            "runtime_abi": 3,
            "fallbacks": result.fallbacks,
            "optimization_report": result.optimization_report,
            "runtime_dependencies": result.runtime_dependencies,
        }
        (directory / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        )
        mapping = {
            name: {
                "source": str(project.modules[name].path),
                "functions": {
                    fn.symbol: {
                        b.name: [{"register": op.result, "line": op.line} for op in b.instructions]
                        for b in fn.blocks
                    }
                    for fn in program.functions
                },
            }
            for name, program in programs.items()
        }
        (directory / "source-map.json").write_text(
            json.dumps({"schema_version": 2, "modules": mapping}, indent=2, sort_keys=True) + "\n"
        )
        if self.emit_ir:
            (directory / "program.cir.json").write_text(
                json.dumps(result.ir, indent=2, sort_keys=True) + "\n"
            )
        if self.emit_ast:
            (directory / "program.ast.txt").write_text(result.analysis["ast"] + "\n")

    def build(self, source=None, *, module=None, output=None) -> Result:
        if self.backend == "native" and self.target != "c":
            from .backend.native.api import build

            return build(self, source, module, output)
        if self.target == "c":
            return self.transpile(source, module=module, output=output)
        result, snapshot, analysis, program = self._prepare(source, module=module)
        if not result.success:
            return result
        config = result.metadata["python"]
        suffix = config["config"]["EXT_SUFFIX"] if self.target == "extension" else ""
        output = Path(
            os.path.abspath(
                output or Path("build") / _entry_stem(program) / (_entry_stem(program) + suffix)
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
        if program[0].entry.package and module_name != program[0].entry.name:
            raise ValueError(
                "a package extension output name must match its top-level package name"
            )
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
                    cache_dir=(
                        Path(self.cache_dir)
                        if self.cache_dir is not None
                        else Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
                        / "calcium"
                        / "objects-v1"
                    )
                    if self.cache
                    else None,
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

    def bundle(
        self, source=None, *, module=None, output=None, resources=(), dependencies=True
    ) -> Result:
        """Build a relocatable Linux folder including its matching CPython runtime."""
        if self.backend == "native":
            raise ValueError(
                "native executables already use standalone CaRT; --bundle is for the cpython backend"
            )
        from .deployment import bundle

        return bundle(
            self,
            source,
            module=module,
            output=output,
            resources=resources,
            dependencies=dependencies,
        )


def _module_name(name):
    return name if re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name) else "calcium_program"


def _entry_stem(plan):
    entry = plan[0].entry
    return (
        entry.name.split(".")[-2] if entry.name.endswith(".__main__") else entry.name.split(".")[-1]
    )
