"""Native orchestration deliberately never queries a target Python SDK."""

import ast
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import warnings
from hashlib import sha256
from pathlib import Path
from time import perf_counter

from ... import __version__
from ...frontend.project import discover
from ...ir.builder import build as build_ir
from ...models import Diagnostic, Result
from ...optimizer import optimize
from ..cache import cached_object
from ..project import module_id
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
        with warnings.catch_warnings(record=True):
            warnings.simplefilter("always", SyntaxWarning)
            project = discover(
                source,
                module=module,
                root=compiler.project_root,
                include_paths=compiler.include_paths,
                include_all=compiler.include_all,
            )
        if project.entry.package:
            raise ValueError(
                "Bonemarrow package executables require __main__.py; use --module package.entry"
            )
        for item in project.modules.values():
            if item.tree is None:
                continue
            for node in ast.walk(item.tree):
                imports = (
                    [a.name for a in node.names]
                    if isinstance(node, ast.Import)
                    else [node.module]
                    if isinstance(node, ast.ImportFrom) and not node.level
                    else []
                )
                for imported in imports:
                    if (
                        imported
                        and imported in project.external
                        and imported.split(".")[0] in sys.stdlib_module_names
                    ):
                        result.diagnostics.append(
                            Diagnostic(
                                "C160",
                                f"Bonemarrow has no native standard-library implementation for {imported!r}; use --cpython for compatibility (no automatic fallback)",
                                str(item.path),
                                node.lineno,
                                node.col_offset + 1,
                            )
                        )
        snapshot = project.entry.source
        result.metadata.update(
            source=str(snapshot.path),
            source_sha256=snapshot.digest,
            engine="Bonemarrow",
            bonemarrow_version=__version__,
        )
        reports, units, irs, asts, decisions_all = [], {}, {}, {}, []
        # The executable __main__ namespace is distinct from an imported copy of
        # the entry module, matching python script.py / python -m package.module.
        imported = {name for names in project.imports.values() for name in names}
        records = [
            (name, item, False)
            for name, item in sorted(project.modules.items())
            if name != "__main__"
            and (
                name != project.entry.name
                or name in imported
                or any(n.startswith(name + ".") for n in imported)
            )
        ]
        records.append(("__main__", project.entry, True))
        infos = []
        if compiler.progress:
            compiler.progress(f"optimizing at O{compiler.optimization}")
        for name, item, entry in records:
            info = {
                "name": name,
                "symbol": "ca_m_" + module_id(name),
                "package": (item.name if item.package else item.name.rpartition(".")[0])
                if not entry or project.module_entry
                else "",
                "is_package": item.package,
                "file": str(item.path),
                "paths": [str(p) for p in item.namespace_paths]
                if item.namespace
                else [str(item.directory)],
                "namespace": item.namespace,
                "script": entry and not project.module_entry,
            }
            infos.append(info)
            if item.namespace:
                tree = original = ast.Module(body=[], type_ignores=[])
                errors = []
            else:
                original, tree, errors = prepare(item.source)
            result.diagnostics.extend(errors)
            if any(d.severity == "error" for d in errors):
                continue
            program = build_ir(tree)
            decisions = optimize(program, compiler.optimization)
            for decision in decisions:
                if "assumption" in decision:
                    decision["assumption"] = decision["assumption"].replace(
                        "overflow/subclasses use CPython",
                        "overflow uses CaRT arbitrary precision; Python subclasses are unsupported",
                    )
                if decision["pass"] == "unboxed-integer-region":
                    decision["boxing"] = "tagged primitive at public return; no numeric heap box"
            text, coverage = generate(program, compiler.optimization, module_info=info)
            units[info["symbol"] + ".c"] = text
            reports.append({"module": name, **coverage})
            irs[name] = program.to_dict()
            asts[name] = ast.dump(original, indent=2, include_attributes=True)
            decisions_all.extend({"module": name, **d} for d in decisions)
        result.diagnostics = list(
            {(d.code, d.message, d.path, d.line, d.column): d for d in result.diagnostics}.values()
        )
        result.optimizations = decisions_all
        result.ir = irs.get("__main__") if len(project.modules) == 1 else {"modules": irs}
        result.analysis = {"ast": asts.get("__main__", ""), "modules": asts}
        report = {
            "schema_version": 1,
            "backend": "native",
            "engine": "Bonemarrow",
            "modules": reports,
            "measurement": "static lowering decisions, not runtime counts",
            "unsupported_features": [
                d.message for d in result.diagnostics if d.severity == "error"
            ],
        }
        # Preserve the single-file optimization report interface.
        if reports:
            report.update({k: v for k, v in reports[-1].items() if k not in {"module"}})
        report["unsupported_features"] = [
            d.message for d in result.diagnostics if d.severity == "error"
        ]
        result.optimization_report = report
        result.metadata["modules"] = [
            {"name": i["name"], "source": i["file"], "namespace": i["namespace"]} for i in infos
        ]
        result.metadata["unresolved_imports"] = sorted(project.external)
        if any(d.severity == "error" for d in result.diagnostics):
            return result, None
        lines = ['#include "cart.h"']
        for info in infos:
            symbol = info["symbol"]
            lines += [
                f"extern CaModule {symbol};",
                f"void {symbol}_roots(CaRuntime *,CaFrame *,CaFrame *);",
            ]
        lines += [
            "int main(void){CaRuntime runtime={0};CaRuntime *rt=&runtime;",
            "CaModule *modules[]={" + ",".join("&" + i["symbol"] for i in infos) + "};",
            f"CaFrame roots[{len(infos) * 2}];",
            f"rt->modules=modules;rt->module_count={len(infos)};",
        ]
        for index, info in enumerate(infos):
            lines += [f"{info['symbol']}_roots(rt,&roots[{index * 2}],&roots[{index * 2 + 1}]);"]
        if project.module_entry:
            parts = project.entry.name.split(".")
            for index in range(1, len(parts)):
                symbol = "ca_m_" + module_id(".".join(parts[:index]))
                lines += [f"ca_module_start(rt,&{symbol});if(rt->error)goto finish;"]
        lines += [
            "ca_module_start(rt,&ca_m_" + module_id("__main__") + ");",
            "finish:;",
            'int status=0;if(rt->error){fprintf(stderr,"%s: %s\\n",rt->error,rt->message);status=1;}',
            "if(fflush(stdout)!=0)status=1;ca_destroy(rt);return status;}",
        ]
        units["program.c"] = "\n".join(lines) + "\n"
        result.runtime_dependencies = ["C11 platform C library", "libm"]
        result.success = True
        return result, (snapshot, units)
    except (SyntaxError, UnicodeError) as exc:
        result.diagnostics.append(
            Diagnostic(
                "C100",
                str(exc),
                getattr(exc, "filename", None) or str(source),
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
    snapshot, units = plan
    runtime = Path(__file__).resolve().parents[2] / "runtime" / "native"
    directory.mkdir()
    for name, text in units.items():
        (directory / name).write_text(text)
    shutil.copyfile(runtime / "include" / "cart.h", directory / "calcium_runtime.h")
    # Use the established owned-output publication mechanism and flat artifacts.
    (directory / "cart.h").write_text('#include "calcium_runtime.h"\n')
    for path in sorted((runtime / "src").glob("*.c")):
        shutil.copyfile(path, directory / ("cart_" + path.name))
    manifest = {
        "generator": "calcium",
        "version": __version__,
        "backend": "native",
        "runtime_abi": 2,
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
    name = (
        module.split(".")[-1]
        if module
        else Path(source).name
        if Path(source).is_dir()
        else snapshot.path.stem
    )
    output = Path(
        os.path.abspath(output or Path("build") / name / (name + ("-c" if transpile else "")))
    )
    if output.is_symlink():
        raise ValueError("output must not be a symlink")
    if transpile and snapshot.path.is_relative_to(output):
        raise ValueError("output directory must not contain the input source")
    if not transpile and (
        output == snapshot.path or output.is_dir() or output.suffix in {".py", ".pyw"}
    ):
        raise ValueError("native output must be a file distinct from input")
    inputs = [Path(m["source"]) for m in result.metadata["modules"]]
    if any(
        (transpile and p.is_relative_to(output.resolve()))
        or (not transpile and p == output.resolve())
        for p in inputs
    ):
        raise ValueError("native output must not overwrite project sources")
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
                flags = [
                    cc,
                    "-std=c11",
                    f"-O{compiler.optimization}",
                    "-fno-fast-math",
                    "-ffp-contract=off",
                    "-I",
                    str(staged),
                ]
                if compiler.debug:
                    flags += ["-g", "-Wall", "-Wextra"]
                elif compiler.cache:
                    flags += [f"-ffile-prefix-map={staged}=/calcium/generated"]
                version = subprocess.check_output(
                    [cc, "--version"], text=True, timeout=15
                ).splitlines()[0]
                identity = {
                    "backend": "bmrw",
                    "runtime_abi": 2,
                    "compiler": version,
                    "compiler_sha256": sha256(Path(cc).read_bytes()).hexdigest(),
                }
                cache = (
                    Path(compiler.cache_dir)
                    if compiler.cache_dir is not None
                    else Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
                    / "calcium"
                    / "objects-native-v2"
                )
                objects, commands, reports, warnings = [], [], [], []
                for path in sorted(staged.glob("*.c")):
                    obj = path.with_suffix(".o")
                    command = [*flags, "-c", str(path), "-o", str(obj)]
                    commands.append(command)

                    def compiling():
                        if compiler.progress:
                            compiler.progress("compiling " + path.name)

                    report = (
                        cached_object(
                            flags,
                            path,
                            obj,
                            directory=staged,
                            cache=cache,
                            identity=identity,
                            on_compile=compiling,
                        )
                        if compiler.cache and not compiler.debug
                        else None
                    )
                    if report is None:
                        compiling()
                        run = subprocess.run(command, capture_output=True, text=True, timeout=120)
                        if run.returncode:
                            raise OSError(f"C compiler failed: {run.stderr}")
                        warnings.append(run.stderr)
                        reports.append({"source": path.name, "hit": False})
                    else:
                        if "run" in report:
                            raise OSError(f"C compiler failed: {report['run'].stderr}")
                        warnings.append(report.pop("warnings"))
                        reports.append({"source": path.name, **report})
                        if report["hit"] and compiler.progress:
                            compiler.progress("reusing " + path.name)
                    objects.append(obj)
                if compiler.progress:
                    compiler.progress("linking executable")
                command = [cc, *(str(p) for p in objects), "-o", str(binary), "-lm"]
                run = subprocess.run(command, capture_output=True, text=True, timeout=120)
                for obj in objects:
                    obj.unlink()
                result.metadata["toolchain"] = {
                    "command": command,
                    "compile_commands": commands,
                    "cache": reports,
                    "warnings": "".join(warnings) + run.stderr,
                    "compiler": version,
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
