"""Toolchain queries are isolated Python subprocesses, never project imports."""

import json
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


class ToolchainError(RuntimeError):
    pass


def python_config(executable: str) -> dict:
    script = """import json, sys, sysconfig, platform
print(json.dumps({"version": list(sys.version_info[:2]), "implementation": sys.implementation.name,
"platform": sys.platform, "machine": platform.machine(), "prefix": sys.base_prefix, "executable": sys.executable,
"include": sysconfig.get_path("include"), "platinclude": sysconfig.get_path("platinclude"),
"config": {k: sysconfig.get_config_var(k) for k in ["LIBDIR", "LDLIBRARY", "LIBS", "SYSLIBS", "LINKFORSHARED", "EXT_SUFFIX", "Py_GIL_DISABLED"]}}))"""
    try:
        run = subprocess.run(
            [executable, "-I", "-c", script], capture_output=True, text=True, timeout=20
        )
        if run.returncode:
            raise ToolchainError(run.stderr.strip() or "target Python configuration query failed")
        config = json.loads(run.stdout)
    except (OSError, subprocess.TimeoutExpired, ValueError) as exc:
        raise ToolchainError(f"cannot query target Python: {exc}") from exc
    if config["implementation"] != "cpython" or config["version"] != list(sys.version_info[:2]):
        raise ToolchainError("host and target must use the same CPython minor version")
    if config["version"] not in [[3, 12], [3, 13], [3, 14]]:
        raise ToolchainError("this prototype targets CPython 3.12–3.14 only")
    if config["config"]["Py_GIL_DISABLED"]:
        raise ToolchainError("free-threaded CPython is not supported")
    if config["platform"] != "linux":
        raise ToolchainError("native builds currently support Linux only")
    return config


def compile_c(
    directory: Path,
    output: Path,
    *,
    config: dict,
    compiler: str | None,
    target: str,
    optimization: int,
    debug: bool,
) -> dict:
    selected = (
        shutil.which(compiler) if compiler else (shutil.which("gcc") or shutil.which("clang"))
    )
    if not selected:
        raise ToolchainError(f"C compiler not found: {compiler or 'gcc or clang'}")
    include = Path(config["include"])
    if not (include / "Python.h").is_file():
        raise ToolchainError(f"Python development headers missing: {include / 'Python.h'}")
    cfg = config["config"]
    command = [
        selected,
        "-std=c11",
        f"-O{optimization}",
        "-fno-fast-math",
        "-ffp-contract=off",
        f"-DCALCIUM_PY_MINOR={config['version'][1]}",
        "-I",
        str(include),
        "-I",
        str(directory),
    ]
    if config["platinclude"] != config["include"]:
        command += ["-I", config["platinclude"]]
    if debug:
        command += ["-g", "-Wall", "-Wextra"]
    if target == "extension":
        command += ["-shared", "-fPIC", "-DCALCIUM_EXTENSION=1"]
    # The emission directory is private and contains only generated sources.
    command += [str(path) for path in sorted(directory.glob("*.c"))] + ["-o", str(output)]
    if target == "executable":
        library = Path(cfg["LIBDIR"] or "") / (cfg["LDLIBRARY"] or "")
        if not library.is_file():
            raise ToolchainError(f"embeddable shared libpython missing: {library}")
        command += [str(library), f"-Wl,-rpath,{library.parent}"]
        command += shlex.split(cfg["LINKFORSHARED"] or "")
    command += shlex.split(cfg["LIBS"] or "") + shlex.split(cfg["SYSLIBS"] or "")
    try:
        version = subprocess.run(
            [selected, "--version"], capture_output=True, text=True, check=True, timeout=15
        ).stdout.splitlines()[0]
        run = subprocess.run(command, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        raise ToolchainError(f"native toolchain failed: {exc}") from exc
    if run.returncode:
        raise ToolchainError(f"C compiler exited {run.returncode}:\n{run.stderr}")
    return {"compiler": version, "command": command, "warnings": run.stderr}
