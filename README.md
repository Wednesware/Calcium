# Calcium

Calcium is Wednesware's Python-to-C compiler. It lowers supported Python through
control-flow IR into C11 and builds executables or CPython extension modules.

**Version 26.11.** Bonemarrow (`bmrw`) is the default independent native backend.
It compiles supported multi-file Python projects without CPython. Select
`--cpython` explicitly for broad compatibility, CPython extensions or bundles.

Build a script or package directly:

```sh
python -m pip install -e .
ca main.py
ca my_package/
```

The default output is `build/<entry>/<entry>`. A package executable needs
`__main__.py`. Select another action with an option:

```sh
ca main.py -o build/myapp
ca main.py --run -- one --another
ca main.py --check --no-fallback
ca main.py --inspect --emit-ir
ca main.py --transpile -o build/c --emit-ir
ca main.py -O2 --report-optimizations
ca my_package/ -O3 --optimization-report
ca main.py --benchmark --repeat 7 --format json
ca --module my_package.cli --project-root ./src
ca --info
```

`--build` is the default. Existing `ca build`, `ca run`, `ca inspect`, and the
other subcommands remain available. `calcium` and `python -m calcium` expose the
same interface. Use `ca --help` for actions and `ca --build --help` for build
options. Runtime arguments follow `--`; only `--run` and `--benchmark` execute
the program. Run propagates its exit status.

## Bonemarrow: standalone native projects

```sh
ca examples/native/hello.py --run
ca main.py --use bmrw -O3 --run --keep-c
ca my_package/ -O3
ca --module my_package.cli --project-root ./src
ca main.py -v --fancy
ca existing_application/ --cpython
```

Calcium is the interface; Bonemarrow is the independent CaRT-based execution
engine. `--bmrw`, `--use bmrw`, and legacy `--backend native` are equivalent.
`--cpython`, `--use cpython`, and `--backend cpython` select compatibility mode.
Conflicting selections fail clearly. Normal compilation stays quiet; verbose
and optional fancy messages go to stderr, never into generated executables.

Bonemarrow supports multi-file projects, classes, closures, exceptions/context
managers, coroutines/await, comprehensions, lazy generator expressions, bytes and selected native
standard-library APIs. SHA-256 and SQLite use their established C libraries.
Unsupported features never fall back to CPython. Discribe's document storage core
has been exercised natively.
Native regex, ZIP, HTTP downloads, subprocesses, threads, traceback formatting and
compiled-module import helpers are now available. Discribe with vendored Nitrogen
builds and runs --help; dynamically downloaded Python and the full GUI/collaboration
runtime still need additional support. See [built-in coverage](docs/BUILTINS.md). Read the [native contract](docs/NATIVE.md),
[library API scope](docs/NATIVE_LIBRARIES.md), and [26.11 notes](docs/releases/26.11.md).

## Installation and deployment

The compiler runs on CPython 3.12–3.14 on Linux. Native builds need GCC or Clang
and the platform C/math libraries, with no Python SDK/runtime dependency in the
output. Native hashlib/sqlite3 builds also require pkg-config and OpenSSL/SQLite
development files and link their C libraries. CPython-backend builds additionally need matching
Python development headers and shared libpython for executables. A relocated
Python installation is supported when its matching library is under its current
prefix. CPython-backend executables require compatible CPython, its standard
library, and external runtime dependencies. Use `--cpython --bundle` to include that runtime
in a relocatable Linux directory; see [Deployment](docs/DEPLOYMENT.md).

Project imports compile recursively into separate C files. Directory inputs
include local Python modules; script inputs follow static imports. Add source
roots with `--include-path` and dynamically selected modules with `--include-all`.
Ordinary executables leave external dependencies and data on the host. Bundles
copy the selected Python environment and package data. See [Packages](docs/PACKAGES.md); those historical importlib details apply to `--cpython`.

Objects are cached by preprocessed C, compiler identity, backend/ABI and flags;
CPython builds additionally fingerprint the Python SDK. Every build relinks against current libraries. Use
`--no-cache` to recompile, `--cache-dir PATH` to choose a location, or `--verbose`
to see compilation/cache-reuse stages. Debug builds bypass the cache. Cache failures do not
block a build. Generated C and IR can be retained using `--keep-c`, `--emit-ir`,
or `--emit-ast`.

## CPython compatibility backend

The following broader lowering/fallback capabilities apply to explicit `--cpython`.
Bonemarrow support is listed above and in its [contract](docs/NATIVE.md).

Native lowering covers literals, containers, arithmetic, chained comparisons,
short circuiting, conditionals, loops, unpacking (including starred assignment),
f-strings, assignment expressions, imports, global bindings, deletion, assertions,
explicit raises, `try`/`except`/`else`/`finally`, synchronous `with`, list/set/dict
comprehensions, dynamic dict keys/set expansion, and calls with positional, named,
`*args`, and `**kwargs` arguments.
Module-level functions support positional-only and keyword-only parameters,
defaults, variadic arguments, recursion, and aliases. Attribute and subscript
augmented assignment evaluate their targets once.

Classes, closures, decorators, annotated functions, generators, async functions,
and function bodies containing unsupported syntax use **reported definition-level
fallback** when allowed. Supported neighboring code remains native; the module
is labeled `mixed`. Unsupported module-level syntax uses whole-module fallback.
No fallback occurs during analysis. With `--cpython`, the CLI allows reported compatibility execution, including projects;
`--no-fallback` requires all-native lowering. With `backend="cpython"`, the API preserves its earlier policy:
isolated scripts allow fallback, and projects require `fallback=True`.
Fallback is CPython execution, not native acceleration.

O0 is the baseline; O1 removes unreachable IR blocks. O2 adds exact-integer
add/subtract/multiply fast paths with checked machine arithmetic and CPython
fallback for overflow, large values, and subclasses. O3 also simplifies branches
on immutable literal constants. Eligible pure-integer functions keep locals and
loop arithmetic in native C scalars, with checked overflow and a generic slow path.
Guarded self-recursion calls the scalar implementation directly. Other operations
remain boxed. General specialization and inlining remain open. All levels preserve assertions and
arbitrary-precision arithmetic. No general speedup is promised.

Native functions remain C builtins: normal Python frames, code objects, exact
function introspection, some argument-error wording, and traceback formatting
differ. Pattern matching, native class/closure/generator machinery, other platforms,
free-threading, subinterpreters, full native-runtime compatibility and a general C ABI
remain future work. The independent runtime currently supports only the native
milestone documented above. Read the [implementation
contract](docs/IMPLEMENTATION.md) and [remaining-work ledger](docs/STATUS.md)
before relying on support.

## Inspect native coverage

`ca check my_package/ -O3 --optimization-report` lists native/fallback function
counts, qualified names, located reasons and enclosing module/class/function
boundaries without executing the application. JSON/API results and C manifests
include the same static report. See [Optimization report](docs/OPTIMIZATION_REPORT.md)
for counting rules and an analysis-only Discribe example.

## Measure computation separately

```sh
ca examples/compute.py --benchmark --function sum_squares --arguments '[1000000]' -O3
ca examples/compute.py --benchmark --function fibonacci --arguments '[25]' -O3
ca examples/compute.py --benchmark --function containers --arguments '[20000]' -O3
```

`--function` excludes startup/imports, validates typed results, output, exceptions
and working-directory file hashes before publishing timings, and reports raw
samples, medians, IQR and whole-process peak RSS. The ordinary benchmark still
measures startup too. [Recorded measurements](docs/BENCHMARKS.md) apply to these
workloads and this machine, not to Nitrogen or every Python program.

## Python API

```python
from calcium import Calcium

result = Calcium(target="executable", optimization=2).build(
    "main.py", output="build/myapp"
)
if result.success:
    print(result.executable)
else:
    for diagnostic in result.diagnostics:
        print(diagnostic.code, diagnostic.message)
```

`analyze`, `inspect`, `transpile`, and `build` return diagnostics, artifacts,
IR, scope/type facts, fallback/optimization reports, and timings. Analysis never
executes source or downloads dependencies. Compilation uses no network services.

## Development

```sh
python -m pip install -e '.[test]' ruff build
python -m pytest -q
ruff check calcium tests
ruff format --check calcium tests examples
python -m build
```

Tests compile real C, compare execution with CPython at O0–O3, import extensions,
and check diagnostics, lifetime behavior, project imports without original sources,
cache behavior, CLI, and packaging. See [Testing](docs/TESTING.md).

The longer-term design remains in [Architecture](docs/ARCHITECTURE.md),
[CIR](docs/IR.md), [Type System](docs/TYPE_SYSTEM.md), [Runtime](docs/RUNTIME.md),
[Optimizations](docs/OPTIMIZATIONS.md), and [Roadmap](docs/ROADMAP.md).
Calcium is separate from Kefir. Magnesium and Iodine are optional future adapters.

Licensed under the [MIT License](LICENSE).
