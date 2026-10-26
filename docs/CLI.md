# CLI and Python API

26.11 defaults to Bonemarrow. Use `--use bmrw` / `--bmrw` / `--backend native`
for explicit native selection, or `--cpython` / `--use cpython` / `--backend cpython`
for compatibility. Conflicts fail. `Calcium()` defaults to `backend="bmrw"`.
See [NATIVE.md](NATIVE.md) for the native support boundary; broader compatibility
options below require selecting CPython. `-v`/`--verbose` reports actual stages
to stderr; `--fancy` explicitly adds an awakening message.

The default action compiles a script or package to an executable:

```sh
ca main.py
ca my_package/
ca main.py -o build/myapp
ca -m my_package.cli --project-root ./src
```

A package executable needs `__main__.py`; a project directory needs `main.py` or
`__main__.py`. The default native output is `build/<entry>/<entry>`.

## Actions

| Action option | Behavior | Executes program? |
| --- | --- | --- |
| `--build` (default) | Build an executable or selected target | No |
| `--run` | Build and execute; pass arguments after `--` | Yes |
| `--check` | Check source, scopes, capability, and IR | No |
| `--inspect` | Print analysis, IR, and requested reports | No |
| `--transpile` | Emit C/runtime/manifest without invoking a C compiler | No |
| `--benchmark` | Validate output and compare repeated CPython/native runs | Yes |
| `--info` | Show runtime and compiler configuration | No |

Action options can precede or follow a source. Select one action. Every action
also retains its old subcommand spelling, such as `ca build main.py` or
`ca inspect main.py`. A source directory named `build` or another command must
use a path, such as `ca ./build`. Options are parsed before `--`; everything after
it is a program argument. `--run` and `--benchmark` accept those arguments.

`ca --help` lists actions. `ca --build --help` lists build options. `ca --version`
prints the version. Neither needs a compiler or Python SDK.

## Options

| Option | Applies to / behavior |
| --- | --- |
| `-o`, `--output PATH` | Build/run/transpile: native file or C-output directory |
| `-m`, `--module NAME` | Module entry; mutually exclusive with source path |
| `--project-root PATH` | Primary project source root |
| `--include-path PATH` | Additional root; repeatable |
| `--include-all` | Include dynamic local modules under primary root |
| `-O0` … `-O3` | Optimization level; default O0; see implementation contract |
| `--python PATH` | Matching target CPython executable |
| `--compiler gcc\|clang` | Native builds only |
| `--bundle` | Build/run: relocatable Linux folder containing CPython |
| `--resource PATH` | Bundle: repeatable extra data path inside a project root |
| `--no-dependencies` | Bundle: copy standard library only |
| `--target c\|executable\|extension` | Build target; default executable; transpile requires C |
| `--allow-fallback` | Explicitly enable reported compatibility execution (already the CLI default) |
| `--no-fallback` | Reject all Python code-object fallback |
| `--keep-c` | Build/run: preserve generated C beside native output |
| `--emit-ir`, `--emit-ast` | Preserve intermediate sidecars; inspect prints them |
| `--no-cache` | Native commands: compile all translation units again |
| `--cache-dir PATH` | Override default `$XDG_CACHE_HOME/calcium/objects-v1` or `~/.cache/calcium/objects-v1` |
| `--debug` | Native debug information; bypass object cache |
| `--verbose` | Print toolchain commands and cache hit/miss metadata |
| `--report-fallbacks` | Print compatibility boundaries and reasons |
| `--report-optimizations` | Print applied passes and their guards |
| `--format text\|json` | Machine-readable results, except run |
| `--warmups N`, `--repeat N`, `--timeout SECONDS` | Benchmark only; defaults 1, 7, 60 |

Native extension output must end in the target interpreter's extension suffix;
its filename determines the module name. General `--target shared` remains
unsupported. Existing inputs, symlink outputs, and foreign output directories
are protected. Failed builds preserve earlier native outputs.

The CLI allows reported compatibility execution by default so ordinary Python
packages build without extra action/policy flags. `--no-fallback` rejects it
everywhere. The API retains strict project defaults unless `fallback=True`. Definition-level
fallback preserves neighboring native definitions and statements; unsupported
module-level syntax selects whole-module fallback. Both are reported.

## Results and exit codes

| Exit status | Meaning |
| --- | --- |
| 0 | Compilation succeeded, possibly with warnings |
| 1 | Source/unsupported-feature error or benchmark mismatch |
| 2 | Invalid command/configuration |
| 3 | Runtime/toolchain/build failure |
| 4 | Internal compiler invariant failure |

After launch, run returns the child's status (signals map to `128 + signal`).
JSON goes to stdout without ANSI. Text diagnostics go to stderr.
Benchmarks compare stdout/stderr/status before publishing timings; workloads
must be repeatable. Filesystem effects are not compared.

## API

```python
from calcium import Calcium

compiler = Calcium(target="executable", optimization=2, cache=True)
result = compiler.build("main.py", output="build/myapp")
print(result.success, result.diagnostics)
```

`Calcium` is immutable. Configuration includes target, optimization, compiler,
Python path, debug/fallback policy, project roots, retention flags, `cache`, and
optional `cache_dir`. `analyze`/`inspect` return structured diagnostics and IR;
`transpile`/`build` add hashed artifacts. All accept a source or `module=`, and
artifact methods accept `output=`. The default API target remains C for compatibility.

Expected source/environment failures produce `success=False`; invalid configuration
raises `ValueError`; compiler invariant failures raise `CalciumInternalError`.
Analysis executes no analyzed source and imports no project dependencies.

Function benchmarks accept `--function NAME`, JSON `--arguments` /
`--keyword-arguments`, and optional `--working-directory PATH` fixture. They time
the call after imports; see [Benchmarks](BENCHMARKS.md).

`Calcium.bundle(source, output="dist/app", resources=(), dependencies=True)`
returns a result whose bundle metadata records its directory. Run its `run`
launcher; `result.executable` is the underlying native binary.

`--optimization-report` prints static module/function coverage, qualified names,
located fallback reasons and optimizer decisions to stderr. JSON output, the API
and emitted manifests expose `optimization_report`; strict rejection still returns
coverage when analysis completes. See [Optimization report](OPTIMIZATION_REPORT.md).
This does not measure startup or runtime hotness.

`--backend cpython` is the default compatibility runtime. `--backend native`
selects the standalone CaRT milestone and always rejects unsupported features,
regardless of `--allow-fallback`. The native backend supports C/executable targets,
retained C/IR/AST and static reports, but not extensions or CPython bundles.
`ca --info --backend native` works without querying a Python SDK.
See [Native backend](NATIVE.md).
