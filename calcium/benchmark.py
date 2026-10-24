"""Separate sustained function computation from process startup/import time."""

import json
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
from dataclasses import replace
from pathlib import Path

from .backend.toolchain import python_config
from .models import Diagnostic

RUNNER = r"""
import contextlib, importlib.util, io, json, os, pathlib, resource, shutil, struct, sys, time
request=json.load(sys.stdin)
sys.path.insert(0, str(pathlib.Path(request['source']).parent))
def freeze(value):
    t=type(value)
    if t is float: return ['float',struct.pack('!d',value).hex()]
    if t in (type(None),bool,int,str): return [t.__name__,value]
    if t is bytes: return ['bytes',value.hex()]
    if t in (list,tuple): return [t.__name__,[freeze(v) for v in value]]
    if t is dict: return ['dict',[[freeze(k),freeze(v)] for k,v in value.items()]]
    if t in (set,frozenset): return [t.__name__,sorted([freeze(v) for v in value],key=lambda v:json.dumps(v,sort_keys=True))]
    raise TypeError('function benchmark results must be builtin scalar/container values')
def snapshot(root):
    import hashlib
    return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob('*')) if p.is_file()}
setup_out,setup_err=io.StringIO(),io.StringIO()
with contextlib.redirect_stdout(setup_out),contextlib.redirect_stderr(setup_err):
    spec=importlib.util.spec_from_file_location('calcium_benchmark_target',request['path'])
    module=importlib.util.module_from_spec(spec)
    sys.modules[spec.name]=module
    spec.loader.exec_module(module)
    function=getattr(module,request['function'])
fixture=pathlib.Path(request['fixture']); work=pathlib.Path(request['work'])
observations=[]; samples=[]
for i in range(request['warmups']+request['repeat']):
    if work.exists(): shutil.rmtree(work)
    shutil.copytree(fixture,work)
    os.chdir(work)
    out,err=io.StringIO(),io.StringIO(); failure=None; value=None
    with contextlib.redirect_stdout(out),contextlib.redirect_stderr(err):
        start=time.perf_counter_ns()
        try: value=function(*request['args'],**request['kwargs'])
        except BaseException as exc: failure=[type(exc).__module__,type(exc).__qualname__,str(exc)]
        elapsed=(time.perf_counter_ns()-start)/1e9
    observation={'result':freeze(value),'exception':failure,'stdout':out.getvalue(),'stderr':err.getvalue(),'files':snapshot(work)}
    if i>=request['warmups']:
        samples.append(elapsed); observations.append(observation)
    os.chdir(work.parent)
rss=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
rss_bytes=int(rss if sys.platform=='darwin' else rss*1024)
json.dump({'samples':samples,'observations':observations,'setup_stdout':setup_out.getvalue(),
           'setup_stderr':setup_err.getvalue(),'peak_rss_bytes':rss_bytes},sys.stdout)
"""


def _summary(samples):
    quartiles = (
        statistics.quantiles(samples, n=4, method="inclusive") if len(samples) > 1 else samples * 3
    )
    return {
        "samples_seconds": samples,
        "median_seconds": statistics.median(samples),
        "iqr_seconds": quartiles[2] - quartiles[0],
        "min_seconds": min(samples),
        "max_seconds": max(samples),
    }


def benchmark_function(
    compiler,
    source,
    *,
    function,
    arguments=(),
    keywords=None,
    repeat=7,
    warmups=1,
    timeout=60,
    working_directory=None,
):
    if repeat < 1 or warmups < 0 or timeout <= 0:
        raise ValueError("repeat/timeout must be positive and warmups nonnegative")
    source = Path(source).resolve()
    if not source.is_file():
        raise ValueError("function benchmarks currently need a Python entry file")
    with tempfile.TemporaryDirectory(prefix="calcium-compute-bench-") as temporary:
        root = Path(temporary)
        extension_compiler = replace(
            compiler, target="extension", keep_c=False, emit_ast=False, emit_ir=False
        )
        config = python_config(str(compiler.python))
        output = root / ("calcium_benchmark_target" + config["config"]["EXT_SUFFIX"])
        result = extension_compiler.build(source, output=output)
        if not result.success:
            return result
        result.artifacts.clear()
        fixture = root / "fixture"
        if working_directory is None:
            fixture.mkdir()
        else:
            shutil.copytree(Path(working_directory).resolve(), fixture)
        requests = []
        for name, path in [("cpython", source), ("calcium", output)]:
            request = {
                "source": str(source),
                "path": str(path),
                "function": function,
                "args": list(arguments),
                "kwargs": keywords or {},
                "repeat": repeat,
                "warmups": warmups,
                "fixture": str(fixture),
                "work": str(root / name),
            }
            try:
                run = subprocess.run(
                    [str(compiler.python), "-I", "-c", RUNNER],
                    input=json.dumps(request),
                    capture_output=True,
                    text=True,
                    timeout=timeout,
                )
            except subprocess.TimeoutExpired:
                result.success = False
                result.diagnostics.append(
                    Diagnostic(
                        "C104",
                        f"{name} function benchmark timed out after {timeout}s",
                        stage="benchmark",
                    )
                )
                return result
            if run.returncode:
                result.success = False
                result.diagnostics.append(
                    Diagnostic(
                        "C104",
                        f"{name} function benchmark runner failed: {run.stderr}",
                        stage="benchmark",
                    )
                )
                return result
            requests.append(json.loads(run.stdout))
        a, b = requests
        if (a["observations"], a["setup_stdout"], a["setup_stderr"]) != (
            b["observations"],
            b["setup_stdout"],
            b["setup_stderr"],
        ):
            result.success = False
            result.diagnostics.append(
                Diagnostic(
                    "C104",
                    "function result/stdout/stderr/filesystem mismatch; no timings published",
                    stage="benchmark",
                )
            )
            return result
        report = {
            "measurement": "function-call wall time; interpreter startup and imports excluded",
            "function": function,
            "arguments": list(arguments),
            "keyword_arguments": keywords or {},
            "cpython": _summary(a["samples"]),
            "calcium": _summary(b["samples"]),
            "peak_rss_bytes": {"cpython": a["peak_rss_bytes"], "calcium": b["peak_rss_bytes"]},
            "rss_scope": "whole benchmark child process; includes interpreter and validation overhead",
            "warmups": warmups,
            "repeat": repeat,
            "correctness": "typed builtin result, exception, stdout/stderr, filesystem file hashes per repetition",
            "filesystem_scope": "private working directory; absolute paths and external side effects are not sandboxed",
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": sys.version,
        }
        report["speedup"] = (
            report["cpython"]["median_seconds"] / report["calcium"]["median_seconds"]
        )
        result.metadata["benchmark"] = report
        return result
