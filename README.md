# Calcium

Calcium is Wednesware's Python-to-C compiler. It lowers a supported
Python subset through a control-flow IR into readable C11 and optionally builds
executables or CPython extension modules with GCC or Clang.

**Version 26.2.** Generated code uses CPython
object APIs; native numeric specialization and general speedups are not yet
implemented. Project imports are recursively compiled into separate C files. Project builds
reject unsupported syntax by default; `--allow-fallback` explicitly enables reported
per-module Python compatibility mode. Isolated single-file inputs retain the earlier
fallback default; `--no-fallback` enforces native lowering everywhere.

## Install and run

Use a conventional GIL-enabled CPython 3.12–3.14 on Linux. Native builds additionally
need GCC or Clang, matching Python development headers, and shared `libpython`
for executables. Differential tests cover Python 3.12, 3.13 and 3.14 locally and in CI.

```sh
python -m pip install -e .
calcium info
calcium check examples/factorial.py --no-fallback
calcium transpile examples/factorial.py --output build/c --emit-ir
calcium build examples/factorial.py --output build/factorial --compiler gcc
./build/factorial
calcium run examples/factorial.py -O1
calcium inspect examples/factorial.py --emit-ir
calcium run examples/dynamic.py --report-fallbacks
```

## Packages and whole projects

```sh
ca transpile path/to/package --output build/c --emit-ir
ca build path/to/package --output build/myapp
ca build --module my_package.cli --project-root ./src --output build/myapp
ca build project/main.py --include-all --output build/myapp
```

Every discovered project source, including package initializers and imported local
modules, gets its own C translation unit. Generated executables link those units and
run without the original Python project sources. Directory inputs include all local
Python modules; script inputs recursively follow static imports. `--include-path`
adds source roots; `--include-all` includes dynamically selected modules. Inspect
`manifest.json` for per-module C filenames, native/fallback status and external
runtime dependencies. See [package transpilation](docs/PACKAGES.md) for details.

`python -m calcium` works directly from a checkout. For script arguments, use
`calcium run script.py -- one --another`. `run` returns the program's exit status.
`transpile` emits `program.c`, `program.h`, separate runtime sources, a manifest
and a source map without invoking a C compiler. Native builds retain all module C/IR/AST
sidecars with `--keep-c`, `--emit-ir` or `--emit-ast`.

```sh
calcium build examples/factorial.py --target extension
calcium benchmark examples/factorial.py --repeat 7 --format json
```

An extension is imported using its generated module name inside a matching
CPython host. It is distinct from a general C shared library, whose ABI is not
implemented. Executables require compatible CPython/stdlib and any runtime
imports; they are not standalone application bundles.

## Python API

```python
from calcium import Calcium

result = Calcium(target="executable", optimization=1).build(
    "examples/factorial.py", output="build/factorial"
)
if result.success:
    print(result.executable)
else:
    for diagnostic in result.diagnostics:
        print(diagnostic.code, diagnostic.message)
```

`analyze`, `inspect`, `transpile` and `build` return structured diagnostics,
artifacts, CIR, scope/type facts, fallback/optimization reports and timings.
Analysis never executes the analyzed program or downloads its dependencies.
There are no AI-service credentials or network requirements for compilation.

## Support and limits

Native lowering covers literals, bindings, containers, arithmetic, comparisons,
calls, attributes/indexing, short circuiting, conditionals, loops, simple functions,
returns, and simple augmented assignments. Arbitrary-precision integer arithmetic
uses CPython APIs. Native functions accept fixed positional-or-keyword parameters;
annotations, defaults, nested functions, classes, exception handlers, generators and async remain unsupported by native
lowering. Projects report errors for these constructs unless compatibility fallback
is explicitly enabled. Ordinary imports are lowered natively.

O0 is the baseline; O1 also removes unreachable CIR blocks. O2/O3, a general C ABI
and standalone runtime are future work.
Native functions are C builtins: Python frames, code objects, exact function
introspection, some argument-error messages and native traceback formatting differ.
Use fallback execution for programs requiring those Python details. Read the
[current implementation contract](docs/IMPLEMENTATION.md) before relying on support.

Benchmarks validate stdout/stderr/exit status before timing and label startup
costs. They execute the program repeatedly and do not compare filesystem effects;
use repeatable workloads. No fixed speedup or full Python compatibility is claimed.

## Development

```sh
python -m pip install -e '.[test]' ruff
python -m pytest -q
ruff check calcium tests
ruff format --check calcium tests examples
```

Tests compile generated C with both available compilers, compare native/fallback
execution with CPython at O0/O1, import extensions, and verify error, reference,
output-protection, CLI and packaging behavior. See [Testing](docs/TESTING.md).

The architecture began with a design review; implementation was subsequently
authorized by the maintainer. See
[Architecture](docs/ARCHITECTURE.md), [CIR](docs/IR.md),
[Type System](docs/TYPE_SYSTEM.md), [Runtime](docs/RUNTIME.md),
[Optimizations](docs/OPTIMIZATIONS.md), [CLI/API](docs/CLI.md),
[Compatibility](docs/COMPATIBILITY.md), and [Roadmap](docs/ROADMAP.md) for the
longer-term design. These design documents include capabilities not yet delivered;
the implementation contract describes what exists today.

Calcium is separate from Kefir, Wednesware's Nuitka-based application builder.
Magnesium and Iodine remain optional future CLI adapters, with no compiler dependency.
Licensing metadata remains unchanged; no license is inferred from sibling projects.
