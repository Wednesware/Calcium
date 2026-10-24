"""Relocatable Linux CPython bundles; the generated program remains native C."""

import json
import os
import re
import shutil
import subprocess
import tempfile
from dataclasses import replace
from hashlib import sha256
from pathlib import Path

from .backend.toolchain import ToolchainError
from .models import Artifact, Diagnostic

LAUNCHER = """#!/bin/sh
set -eu
root=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd)
export CALCIUM_BUNDLE_ROOT="$root"
export CALCIUM_BUNDLE_HOME="$root/runtime"
export CALCIUM_BUNDLE_PYTHON="$root/runtime/bin/python3"
export LD_LIBRARY_PATH="$root/lib"
for directory in "$root"/runtime/lib/tcl*; do
    if [ -f "$directory/init.tcl" ]; then export TCL_LIBRARY="$directory"; fi
done
for directory in "$root"/runtime/lib/tk*; do
    if [ -f "$directory/tk.tcl" ]; then export TK_LIBRARY="$directory"; fi
done
exec "$root/program" "$@"
"""


def _ignore(directory, names):
    return [name for name in names if name in {"__pycache__", "site-packages", "test", "tests"}]


def _copy_libraries(root, binaries, library_paths):
    """Copy transitive ELF dependencies except the target OS C runtime/loader."""
    core = re.compile(r"^(ld-linux.*|lib(c|m|dl|pthread|rt|util|resolv)\.so(?:\..*)?)$")
    seen = set()
    pending = list(binaries)
    while pending:
        binary = pending.pop()
        identity = binary.stat().st_ino, binary.stat().st_dev
        if identity in seen:
            continue
        seen.add(identity)
        environment = dict(
            os.environ,
            LD_LIBRARY_PATH=os.pathsep.join([str(root / "lib"), *map(str, library_paths)]),
        )
        run = subprocess.run(
            ["ldd", str(binary)], capture_output=True, text=True, timeout=20, env=environment
        )
        output = run.stdout + run.stderr
        if "not found" in output:
            raise ToolchainError(f"unresolved ELF dependency for {binary}: {output}")
        if (
            run.returncode
            and "not a dynamic executable" not in output
            and "statically linked" not in output
        ):
            raise ToolchainError(f"cannot inspect ELF dependencies for {binary}: {output}")
        for line in run.stdout.splitlines():
            match = re.match(r"\s*(\S+) => (/\S+) \(", line)
            if not match:
                continue
            name, path = match.groups()
            if core.fullmatch(name):
                continue
            destination = root / "lib" / name
            source = Path(path)
            if destination.exists():
                if (
                    sha256(destination.read_bytes()).digest()
                    != sha256(source.read_bytes()).digest()
                ):
                    raise ToolchainError(f"conflicting bundled shared library: {name}")
            else:
                shutil.copy2(source, destination)
                pending.append(destination)


def _copy_resources(root, metadata, resources):
    roots = [Path(p) for p in metadata["project_roots"]]
    # Packages get their non-code data tree. Files beside a script are explicit,
    # avoiding an accidental copy of its entire containing directory.
    candidates = {
        Path(path)
        for m in metadata["modules"]
        if m["package"]
        for path in (m["search_locations"] or [m["directory"]])
    }
    candidates.update(Path(p).resolve() for p in resources)
    if any(root.is_relative_to(p) for p in candidates):
        raise ValueError("bundle output must be outside resource directories")
    ignored = {".git", ".hg", ".svn", ".venv", "venv", "__pycache__", "build", "dist"}
    for candidate in sorted(candidates):
        matches = [(len(str(p)), i, p) for i, p in enumerate(roots) if candidate.is_relative_to(p)]
        if not matches:
            raise ValueError(f"resource must be inside a project root: {candidate}")
        _, index, source_root = max(matches)
        if not candidate.exists():
            raise ValueError(f"resource does not exist: {candidate}")
        files = candidate.rglob("*") if candidate.is_dir() else [candidate]
        for source in files:
            relative = source.relative_to(source_root)
            if any(part in ignored or part.startswith(".") for part in relative.parts):
                continue
            if source.is_symlink():
                raise ValueError(f"resource symlinks require an explicit regular copy: {source}")
            if not source.is_file() or source.suffix in {".py", ".pyw", ".pyc", ".pyo"}:
                continue
            target = root / "app" / f"source{index}" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    for index in range(len(roots)):
        (root / "app" / f"source{index}").mkdir(parents=True, exist_ok=True)


def bundle(compiler, source=None, *, module=None, output=None, resources=(), dependencies=True):
    output = Path(os.path.abspath(output or "build/calcium-bundle"))
    if output.is_symlink() or output.exists():
        raise ValueError("bundle output must be a new directory")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".calcium-bundle-", dir=output.parent) as temporary:
        staged = Path(temporary) / "bundle"
        staged.mkdir()
        executable = replace(
            compiler, target="executable", keep_c=False, emit_ir=False, emit_ast=False
        )
        result = executable.build(source, module=module, output=staged / "program")
        if not result.success:
            return result
        result.artifacts.clear()
        config = result.metadata["python"]
        try:
            runtime = staged / "runtime"
            (runtime / "bin").mkdir(parents=True)
            (staged / "lib").mkdir()
            shutil.copy2(Path(config["base_executable"]).resolve(), runtime / "bin" / "python3")
            version = ".".join(map(str, config["version"]))
            stdlib = runtime / "lib" / f"python{version}"
            shutil.copytree(config["stdlib"], stdlib, ignore=_ignore, symlinks=False)
            prefix_lib = Path(config["prefix"]) / "lib"
            for original in prefix_lib.iterdir():
                if original.is_dir() and original.name.startswith(("tcl", "tk", "itcl", "thread")):
                    shutil.copytree(original, runtime / "lib" / original.name, symlinks=False)
            site = stdlib / "site-packages"
            site.mkdir(exist_ok=True)
            if dependencies:
                for original in dict.fromkeys([config["purelib"], config["platlib"]]):
                    if Path(original).is_dir():
                        shutil.copytree(
                            original,
                            site,
                            dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("__pycache__"),
                            symlinks=False,
                        )
            for path in site.glob("*.pth"):
                # Executable .pth lines remain the selected environment's trusted
                # startup hooks. Absolute paths cannot silently depend on the host.
                lines = [
                    line
                    for line in path.read_text().splitlines()
                    if not line
                    or line.startswith("#")
                    or line.startswith("import ")
                    or not Path(line).is_absolute()
                ]
                path.write_text("\n".join(lines) + "\n")
            _copy_resources(staged, result.metadata, resources)
            binaries = [staged / "program", runtime / "bin" / "python3"]
            binaries.extend(p for p in runtime.rglob("*") if p.is_file() and ".so" in p.name)
            _copy_libraries(
                staged, binaries, [prefix_lib, Path(config["config"]["LIBDIR"] or prefix_lib)]
            )
            launcher = staged / "run"
            launcher.write_text(LAUNCHER)
            launcher.chmod(0o755)
            manifest = {
                "generator": "calcium",
                "kind": "cpython-bundle",
                "schema_version": 1,
                "python": config["version"],
                "platform": config["platform"],
                "machine": config["machine"],
                "dependencies": "selected interpreter site-packages"
                if dependencies
                else "standard library only",
                "system_requirements": ["compatible Linux C runtime and ELF loader", "POSIX shell"],
                "files": {
                    str(p.relative_to(staged)): sha256(p.read_bytes()).hexdigest()
                    for p in sorted(staged.rglob("*"))
                    if p.is_file()
                },
            }
            (staged / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            staged.rename(output)
        except (OSError, ToolchainError, subprocess.TimeoutExpired) as exc:
            result.success = False
            result.diagnostics.append(Diagnostic("C303", str(exc), stage="bundle"))
            return result
        result.runtime_dependencies = manifest["system_requirements"]
        result.metadata["bundle"] = {k: v for k, v in manifest.items() if k != "files"}
        result.metadata["bundle"]["directory"] = str(output)
        for name, kind in [
            ("run", "launcher"),
            ("program", "executable"),
            ("manifest.json", "bundle-manifest"),
        ]:
            path = output / name
            result.artifacts.append(
                Artifact(kind, str(path), sha256(path.read_bytes()).hexdigest())
            )
        return result
