# Proposed CLI and Python API

Status: design only. None of the commands or API examples below are implemented.
The CLI is noninteractive by default. Compiler internals return diagnostics;
presentation is a separate adapter. See [Architecture](ARCHITECTURE.md).

## Commands

| Command | Contract | Executes analyzed program? |
| --- | --- | --- |
| `calcium transpile main.py` | Analyze/lower and emit complete C artifact directory; no C compiler required | No |
| `calcium build main.py` | Transpile, probe toolchain and compile requested native artifact | No |
| `calcium run main.py -- args` | Build executable then launch with inherited streams and specified arguments | Yes |
| `calcium check main.py` | Parse, resolve scopes, infer facts and validate lowering capability; diagnostics only | No |
| `calcium inspect main.py` | Print analysis, selected CIR and requested reports; no artifact directory by default | No |
| `calcium benchmark main.py` | Validate observable equivalence, then measure CPython/native runs | Yes, repeatedly |
| `calcium info` | Print Calcium/host/target/runtime/toolchain availability | No user program |

`check` success means the selected support/fallback policy accepts the input; it
is not proof of runtime success. Known missing external dependencies are reported
without importing them. `info` reports unvalidated configurations as such.
`--help` and `--version` do not require Python development headers or a C compiler.

Project entry: `calcium build --project-root ./src --module package.main`.
A source path and `--module` are mutually exclusive. A bare directory requires
an explicit module entry. A script has script semantics; module entry has `-m`
semantics, including package metadata. Project roots also constrain static source
discovery; they do not disable runtime imports outside the root.

## Options and applicability

| Option | Proposed behavior |
| --- | --- |
| `-O0` … `-O3` | Default O0 until validated optimizer release; unavailable levels fail explicitly |
| `--output PATH` | C target: directory; native target: final artifact path; never overwrite sources |
| `--target c\|executable\|extension\|shared` | Default `c` for transpile, executable for build; shared reserved and initially rejected |
| `--compiler gcc\|clang` | Native commands only; explicit choice overrides automatic GCC-then-Clang search |
| `--python PATH` | Select target CPython executable; query version/config without executing user sources |
| `--debug` | Source comments, mapping metadata and native debug info; no changed Python semantics |
| `--verbose` | Compiler-stage details and redacted toolchain commands on stderr |
| `--keep-c` | Preserve intermediate generated directory after successful native build |
| `--emit-ir` | Write versioned JSON and human-readable core CIR sidecars; inspect prints CIR |
| `--emit-ast` | Write target-version AST dump sidecar; inspect prints AST |
| `--report-optimizations` | Decisions including declined transformations, evidence and source locations |
| `--report-fallbacks` | Each fallback boundary/reason and static coverage, not a speed estimate |
| `--format text\|json` | Structured analysis/diagnostics/results for check, inspect, info and benchmark |
| `--no-fallback` | Reject any operation requiring CPython code-object fallback; ordinary object API operations remain allowed |
| `--project-root PATH`, `--module NAME` | Explicit project source root and module entry |
| `--help`, `--version` | Immediate information, no compilation |

`transpile --target executable` is a usage error; it never secretly invokes a
compiler. `build --target c` delegates to C emission without compiler probing;
`--compiler` is then a usage error. `run` and `benchmark` require executable target.
`check` and `inspect` accept analysis, optimization and report options; native
compiler/target/output/keep-c options are usage errors. AST/IR sidecars require
an artifact-producing command or inspect. `--keep-c` on transpile is redundant
and rejected. No option is silently ignored.

Default C output is `build/<entry>/`; native output is `build/<entry>/<entry>`
with platform suffix when required. Intermediate C for a native output is stored
in a separate temporary directory, or `<output>.calcium/` with keep-c. Sidecars
follow that directory. Extension suffix comes from selected Python `sysconfig`;
module export naming must match the import name. Reject incompatible entry names.

Parse program arguments only after `--`. `run` propagates normal child exit
status; on POSIX, signal termination maps to shell convention `128 + signal`.
Compilation errors are rendered before launching, without invoking user code.
Benchmark-specific proposal: `--warmups N`, `--repeat N` (positive), and
`--timeout SECONDS`, with defaults 1, 7 and 60. Each workload is expected to be
repeatable; the command warns that it runs arbitrary user code repeatedly.
Benchmark JSON contains both timings and correctness evidence; no timing is
published if correctness validation fails. See [Testing](TESTING.md).

## Diagnostics and exit codes

A diagnostic has `code`, `severity`, `stage`, `message`, logical path,
start/end line and column, notes and optional help. User-facing columns are
one-based Unicode character columns, converted from AST byte offsets. Internal
spans retain byte offsets for lossless source mapping. Syntax errors retain the
original source excerpt; paths are not C symbol identities.

| Exit | Meaning for compiler commands |
| --- | --- |
| 0 | Success, possibly with warnings |
| 1 | Source/semantic/unsupported-feature error, or benchmark behavior mismatch |
| 2 | Invalid command or configuration |
| 3 | Missing runtime/toolchain/dependency or native build failure |
| 4 | Internal compiler invariant failure |

`run` child status overrides this compiler-command scheme after a successful build;
its status cannot alone distinguish a user exit code from a compiler failure.
Diagnostics identify whether launch occurred. JSON is schema-versioned and has
no ANSI or progress text mixed into stdout. Diagnostics/progress go to stderr
for text commands; machine result objects include their diagnostics. Do not
print arbitrary tracebacks on expected user errors; internal debug traces remain
available with explicit debug/verbose handling.

## Python API

Proposed, not currently importable from this design repository:

```python
from calcium import Calcium

compiler = Calcium(optimization=0, target="c", python="/path/to/python3.12")
result = compiler.transpile("main.py", output="build/main")
if not result.success:
    for diagnostic in result.diagnostics:
        print(diagnostic.message)
```

Configuration fields: optimization, target, compiler, python, debug, fallback
policy and project_root. Validate combinations before opening output paths.
Configuration is immutable; per-call output and entry arguments do not mutate a
shared compiler instance. No API call configures a cloud/AI client or consumes a
network service. This project does not incorporate the unrelated local Gemini
client found during repository inspection.

| Operation | Proposed return |
| --- | --- |
| `analyze(source=None, *, module=None)` | `AnalysisResult` with scopes, graph, facts, diagnostics and capability classification |
| `inspect(source=None, *, module=None)` | `InspectionResult` adding versioned AST/CIR and optimization/fallback reports |
| `transpile(source=None, *, module=None, output=None)` | `CompilationResult` for complete C sources/runtime/maps/manifests |
| `build(source=None, *, module=None, output=None)` | `CompilationResult` adding native artifact and toolchain metadata |

Common result fields: `schema_version`, `success`, immutable diagnostics,
artifacts, optimization decisions, fallback records, runtime dependencies,
metadata and timings. `Artifact` includes kind, path, content hash and target.
Metadata includes Calcium version, target Python/ABI/platform, source hashes,
configuration and reproducibility inputs. Timings map stage names to monotonic
elapsed seconds and are excluded from deterministic source fingerprints.
Analysis results have an empty artifact collection, not fake artifact paths.

Expected parse/compile/environment failures return `success=False` with structured
diagnostics; partial outputs remain unpublished and artifacts enumerate only
valid retained files. Invalid API argument combinations raise `ValueError`;
programming errors/invariant violations raise `CalciumInternalError`, never a
false success. Importing Calcium performs no analysis, target initialization,
terminal manipulation or dependency downloads. Concurrent builds use distinct
work directories; shared caching requires locking and validated content keys.

The public result schema can remain stable while AST/CIR dump schemas version
independently. Do not expose internal mutable graphs as an accidental stable API.
