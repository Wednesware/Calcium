# Calcium

Calcium is Wednesware's Python-to-C compiler. It lowers supported Python through
control-flow IR into C11 and builds executables or CPython extension modules.

**Version 26.3.** Build a script or package directly:

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
ca main.py --benchmark --repeat 7 --format json
ca --module my_package.cli --project-root ./src
ca --info
```

`--build` is the default. Existing `ca build`, `ca run`, `ca inspect`, and the
other subcommands remain available. `calcium` and `python -m calcium` expose the
same interface. Use `ca --help` for actions and `ca --build --help` for build
options. Runtime arguments follow `--`; only `--run` and `--benchmark` execute
the program. Run propagates its exit status.

## Installation and deployment

Use GIL-enabled CPython 3.12–3.14 on Linux. Builds need GCC or Clang, matching
Python development headers, and shared libpython for executables. A relocated
Python installation is supported when its matching library is under its current
prefix. Generated executables still require compatible CPython, its standard
library, and external runtime dependencies. They are not standalone bundles.

Project imports compile recursively into separate C files. Directory inputs
include local Python modules; script inputs follow static imports. Add source
roots with `--include-path` and dynamically selected modules with `--include-all`.
External dependencies and data resources are not bundled. See [Packages](docs/PACKAGES.md).

Native objects are cached by preprocessed C, compiler configuration, Python SDK,
and optimization flags. Every build relinks against current libraries. Use
`--no-cache` to recompile, `--cache-dir PATH` to choose a location, or `--verbose`
to see hit/miss reports. Debug builds bypass the cache. Cache failures do not
block a build. Generated C and IR can be retained using `--keep-c`, `--emit-ir`,
or `--emit-ast`.

## Language support

Native lowering covers literals, containers, arithmetic, chained comparisons,
short circuiting, conditionals, loops, unpacking (including starred assignment),
f-strings, assignment expressions, imports, global bindings, deletion, assertions,
explicit raises, and calls with positional, named, `*args`, and `**kwargs` arguments.
Module-level functions support positional-only and keyword-only parameters,
defaults, variadic arguments, recursion, and aliases. Attribute and subscript
augmented assignment evaluate their targets once.

Classes, closures, decorators, annotated functions, generators, async functions,
and function bodies containing unsupported syntax use **reported definition-level
fallback** when allowed. Supported neighboring code remains native; the module
is labeled `mixed`. Unsupported module-level syntax uses whole-module fallback.
No fallback occurs during analysis. The CLI allows reported compatibility execution by default, including projects;
`--no-fallback` requires all-native lowering. The API preserves its earlier policy:
isolated scripts allow fallback, and projects require `fallback=True`.
Fallback is CPython execution, not native acceleration.

O0 is the baseline; O1 removes unreachable IR blocks. O2 adds exact-integer
add/subtract/multiply fast paths with checked machine arithmetic and CPython
fallback for overflow, large values, and subclasses. O3 also simplifies branches
on immutable literal constants. Values remain boxed at operation boundaries;
unboxed numerical regions are future work. All levels preserve assertions and
arbitrary-precision arithmetic. No general speedup is promised.

Native functions remain C builtins: normal Python frames, code objects, exact
function introspection, some argument-error wording, and traceback formatting
differ. Native exception handlers, comprehensions, pattern matching, dynamic
class machinery, other platforms, free-threading, subinterpreters, standalone
runtime, and a general C ABI remain future work. Read the [implementation
contract](docs/IMPLEMENTATION.md) and [remaining-work ledger](docs/STATUS.md)
before relying on support.

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
