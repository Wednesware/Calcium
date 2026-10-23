# Calcium 0.1.0 implementation contract

This is the current delivery contract. The other specification documents describe
the broader architecture and future milestones. Implementation began on the user's
explicit instruction, “Please generate Calcium itself now,” on 2026-10-23.

## What runs today

Calcium reads Python source without importing it, validates syntax and scope rules
using the selected CPython minor version, classifies native capability, lowers to
CIR, validates ownership/control flow, optionally removes unreachable blocks, emits
C11 plus a separate runtime, and invokes GCC/Clang only for native builds.

The C backend consumes CIR, never Python AST nodes. The IR contains named basic
blocks, object registers, explicit mutable binding slots, operations with error
edges and jump/branch/iterator/return terminators. An ownership dataflow check
rejects use-after-release and inconsistent live registers at joins. C temporaries
start NULL; normal paths release expressions, transfer returns and clear bindings.
Error cleanup saves/restores the pending Python exception while releasing owned
references. Function cleanup order is part of CIR and follows the target minor:
CPython 3.12/3.13 clear declared locals forward; 3.14 clears them in reverse.
Nested boolean value lowering also follows the target's observable truth-testing
behavior. Functions participate in CPython's recursion guard.

The initial representation is uniformly `PyObject *`. Inference reports literal
facts and conservative assignment/branch/loop unions; annotations never authorize
native types. Scope, source hash, CIR, optimizer decisions and fallback reasons
are available through inspect/API/sidecars. Source-map entries connect CIR registers
and blocks to source lines; full Python frame/traceback emulation is not implemented.

The frontend uses CPython's nonexecuting compiler to recognize safely folded
immutable constant expressions, preserving the target's folding decisions.
A module context owns a shared constant pool, including immutable tuples and
identifier-like string interning, so repeated literal loads do not arbitrarily
create distinct objects. This uses no source evaluation. The pool currently holds
all compiled constants while its module/function contexts remain alive, which can
retain more memory than individual Python code objects.

## Native language subset

| Feature | Implemented behavior |
| --- | --- |
| Constants | None, bool, arbitrary-size int, float, str, bytes; immutable tuple constants |
| Bindings | Module/global lookup with builtins fallback; fixed function locals and unbound checks |
| Arithmetic | Python numeric protocols for binary and unary operations, including division, powers, shifts and bitwise operators |
| Comparisons | One comparison at a time, identity, membership and rich comparisons; no chained native comparisons yet |
| Expressions | Calls with positional/named arguments, attributes, indexing/slicing, conditional expressions, short circuiting |
| Containers | List, tuple, set and dict literals; indexed/attribute stores; dynamic dictionary keys conservatively select fallback |
| Control flow | If/else, while/for including else, break, continue, return and pass |
| Functions | Module-level undecorated definitions; fixed positional-or-keyword parameters, recursion and aliases |
| Augmented assignment | Simple name target, Python in-place numeric/sequence protocol |

Defaults, positional-only/keyword-only signatures, star argument expansion,
annotations, nested functions, decorators, imports, classes, destructuring,
comprehensions, exception statements/handlers, generators, async, pattern matching
and unsupported operators select a **whole-module fallback**. Native C functions
and runtime object API operations still use CPython; “native” describes lowering,
not a standalone runtime or unboxed arithmetic.

Fallback compiles the embedded original source text as a code object in the
runtime interpreter and evaluates it in the actual module dictionary. It preserves
ordinary dynamic language execution, imports, hooks, exceptions and reflection
within that Python code. Each reason is reported and the manifest records mode
`fallback`; this is not native acceleration. `--no-fallback` turns those reasons
into located compilation errors. Local imports remain runtime filesystem imports;
only the entry source is embedded, and dependencies/resources are not bundled.

Script `__file__`/`sys.argv[0]` use the absolute source path, and its original
parent directory is placed first on the runtime search path. The working directory
is inherited. Interpreter path initialization uses the selected Python executable,
including its virtualenv and site-packages, instead of guessing a Python prefix
from the generated binary location. That Python installation must remain available. The artifact therefore retains source-directory dependencies and is
not a portable package bundle. Python executable/loader/import-path implementation
details may differ from `python script.py`; these are not claimed identical.

## Native compatibility exclusions

Native functions are `PyCFunction` wrappers with a GC-managed context containing
module globals and constants. They are not `types.FunctionType`, do not expose
normal `__code__`/closure/default metadata and do not create ordinary Python frames.
Some argument TypeError wording, tracing/profiling, recursion-depth thresholds,
loader metadata and traceback presentation consequently differ.

Obvious frame-sensitive builtins and function metadata select fallback, but
static detection cannot find indirect reflection through every alias or external
call. Programs requiring exact frame/function/introspection behavior should use
whole-module fallback, such as an entry script importing their actual Python code,
or run directly under CPython. These exclusions apply even if inspect reports no
obvious reflective construct. O1 does not relax arithmetic or ownership rules.

CPython protocols handle overloaded operators when values reach native operations,
but arbitrary dynamic behavior through builtin-created classes has not received
comprehensive equivalence testing. Full compatibility, exact allocation statistics,
resource-exhaustion parity, weakref/finalizer parity for every native edge case and
thread/subinterpreter support are not claimed. Unsupported syntax is never silently
skipped. An analysis success means accepted lowering/fallback, not runtime success.

## Targets and optimization

- **C**: complete runtime sources/header, `program.c`/`program.h`, manifest and
  source map. No C compiler required to emit it; target Python is queried.
- **Executable**: C11 compiled and linked to the matching shared libpython;
  initializes conventional CPython, executes entry code and finalizes it.
- **Extension**: platform/ABI-suffixed module with `PyInit_<name>` and multi-phase
  execution in its host interpreter. No host initialization/finalization.

Native builds currently require Linux, a matching host/target CPython minor in
3.12–3.14, a GIL-enabled build, compatible SDK/runtime and GCC or Clang. The regression suite is run on Linux x86-64 across CPython 3.12, 3.13 and 3.14;
the GitHub Actions matrix checks all three minors. Header version checks prevent mixing a generated program with a
different minor. Cross compilation, free-threading and subinterpreters are not
supported. Extension module names must be ASCII identifiers matching the output
filename; `--target shared` rejects a general C ABI rather than confusing it with
an extension.

O0 emits required lowering. O1 removes unreachable blocks with verifier checks;
it does not introduce speculative numerical optimizations. O2/O3 fail explicitly.
Native arithmetic uses CPython APIs and can incur additional overhead. No performance
improvement is promised. Generated sources are deterministic for identical source
locations, target/configuration and input bytes; native binaries depend on the
host toolchain and are not claimed reproducible across hosts.

## Commands and results

Implemented commands: `transpile`, `build`, `run`, `check`, `inspect`, `benchmark`,
`info`, `--version` and `--help`. Script arguments follow `--` for run/benchmark.
Use each command's `--help` for implemented options. Native project/module entry
options and interactive initialization are not implemented; argparse rejects them.

`--output` is a directory for target C and a file for native output. An extension
output must carry the target interpreter's extension suffix; its filename controls
the import name. `--emit-ir`/`--emit-ast` preserve intermediate directories during
native builds as well as writing emission sidecars; `--keep-c` retains all C sources.
Existing source files, symlink outputs and foreign output directories are rejected.
Emission stages complete directories before replacement, with rollback. Native
artifacts are atomically replaced only after successful compilation; old outputs
survive failed builds. Intermediate retention and native replacement are separate
transactions; valid retained files can remain when final replacement fails.

API configuration is immutable. `analyze`/`inspect` return `Result` with diagnostics,
analysis and CIR; `transpile`/`build` add hashed artifacts. `to_dict` yields the JSON
schema. Expected source/environment failures return `success=False`; invalid
configuration raises `ValueError`; invalid compiler invariants raise
`CalciumInternalError`. CLI error statuses are 1 source/behavior, 2 configuration,
3 environment/toolchain and 4 internal. After successful launch, run propagates
child status (POSIX signals use 128 + signal). Machine output contains no ANSI.
No compiler command installs dependencies or makes network calls.

Benchmarks compare stdout/stderr/exit status before measurements and on every
repetition. A mismatch produces no timings. End-to-end subprocess samples include
startup and report raw samples, medians, ranges and their ratio. They do not compare
filesystem effects or measure peak RSS, kernel-only computation or fallback
instruction percentages yet. Use repeatable workloads without stateful side effects.

## Validation and remaining work

The regression suite compiles real generated C at both implemented levels with
GCC/Clang when available, compares with CPython, checks error classes, native and
fallback extension loading, repeated-call reference counts, arbitrary precision,
constant identity, source encoding, imports/argv, output protection, IR rejection,
CLI behavior, benchmark mismatch handling and a seeded arithmetic corpus.
A local UBSan smoke run checks generated factorial code; this is not a comprehensive
sanitizer campaign. Wheel verification checks bundled runtime C/header files and
an installed-package smoke build outside the checkout.

Next milestones are richer CIR verification and dataflow, native import/package
execution, more complete exception/lifetime/finalizer tests, function wrapper
metadata, checked unboxed numeric regions, and additional optimization passes.
The longer-term [Roadmap](ROADMAP.md) remains the direction rather than a claim
that every phase has been completed. Magnesium/Iodine are not required: their
compatibility issue remains documented in [Architecture](ARCHITECTURE.md).
