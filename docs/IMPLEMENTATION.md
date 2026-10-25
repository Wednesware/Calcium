# Calcium 26.7 implementation contract

This is the current delivery contract. [Package transpilation](PACKAGES.md)
describes the project/compiler-import additions in 26.2. The other specification documents describe
the broader architecture and future milestones. Implementation began on the user's
explicit instruction, “Please generate Calcium itself now,” on 2026-10-23.

## Backend selection in 26.7

The established behavior below describes explicit `--cpython` compatibility.
Bonemarrow (`--use bmrw`, legacy `--backend native`) is the 26.7 default,
with independent CaRT/C11 execution for packages and a growing language subset,
with no CPython runtime/SDK dependency or fallback. It reuses the frontend/CIR and
validated scalar kernel IR; its values, ownership, errors and limits are specified
in the [native contract](NATIVE.md). See [native benchmarks](NATIVE_BENCHMARKS.md).

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

## 26.5 delivery

Version 26.5 adds `--optimization-report`: static module/function coverage,
qualified source locations, categorized fallback reasons and known enclosing
blocking boundaries. The same report is available through JSON, the Python API
and emitted manifests, including strict-mode rejection and partial-analysis
coverage where available. This release improves diagnostic visibility; it does
not add native language support or runtime speedups. See the
[release notes](releases/26.5.md) and [report reference](OPTIMIZATION_REPORT.md).

## Features retained from 26.4

Version 26.4 adds checked unboxed integer kernels and guarded native self-recursion
at O2/O3, native structured exception handling/context managers/comprehensions,
function-body benchmarks, and relocatable Linux CPython bundles. See the
[release notes](releases/26.4.md), [benchmarks](BENCHMARKS.md) and
[deployment guide](DEPLOYMENT.md). The details and exclusions below define
the delivered scope.

## Features retained from 26.3

Path-first CLI (`ca main.py`, `ca my_package/`) builds by default. Actions are
options (`--inspect`, `--transpile`, `--run`, and others); legacy commands work.
The [CLI reference](CLI.md) describes action options and output locations.
Native object caching fingerprints preprocessed C (including actual system/SDK
headers), compiler identity/configuration, target Python and flags. Private staging
paths are normalized with the compiler's file-prefix map. Builds always relink;
cache writes stage complete entries, reads verify object hashes, and cache failures
fall back to ordinary compilation. Debug builds bypass caching. This is an object
cache, not a claim of cross-host reproducible binaries. `--no-cache` and
`--cache-dir` control reuse. Relocated CPython installations can find matching
libpython under their queried prefix when the recorded build-time LIBDIR is stale.

The project is licensed under MIT. See [remaining work](STATUS.md) for the limits
that this release does not claim to remove.

## Native language subset

| Feature | Implemented behavior |
| --- | --- |
| Constants | None, bool, arbitrary-size int, float, str, bytes; immutable tuple constants |
| Bindings | Module/global lookup with builtins fallback; fixed function locals and unbound checks |
| Arithmetic | Python numeric protocols for binary and unary operations, including division, powers, shifts and bitwise operators |
| Comparisons | Chained comparisons, identity, membership and rich comparisons; short circuiting evaluates operands once and preserves rich-comparison result objects |
| Expressions | Calls with positional/named/expanded arguments, attributes, indexing/slicing, f-strings, assignment expressions, conditional expressions, short circuiting |
| Containers | List, tuple, set and dict literals; list/tuple/dict expansion; indexed/attribute stores; nested/starred unpacking; dynamic dictionary keys and set expansion |
| Control flow | If/else, while/for including else, break, continue, return, pass, assert, explicit/bare raise, try/except/else/finally and synchronous with |
| Deletion/global | Names, attributes and subscripts; global declaration |
| Functions | Module-level undecorated definitions; positional-only/keyword-only parameters, defaults, variadic args/kwargs, recursion and aliases |
| Augmented assignment | Names, attributes and subscripts; targets evaluated once; Python in-place protocols |

List/set/dict comprehensions use native iterator CFGs and private target slots.
Exception edges enter cleanup landing pads, restore handled exception state, and
route return/break/continue through pending finally/with actions. Exception groups
(`except*`) and async comprehensions/context managers still need fallback.

Annotations, nested functions, decorators, classes, generators, async, pattern matching and unsupported
operators require compatibility execution. Unsupported module-level function,
async-function and class definitions use **definition-level fallback**, preserving
native neighboring statements and functions. Reasons carry `boundary: definition`
and the definition's name; the program and module manifest mode is `mixed`.
Native wrappers call Python definitions through ordinary CPython protocols.

The original complete definition (including decorators and original line offsets)
is compiled and executed once in the module globals at its original execution
point. Its body, closures, annotations, decorators and class protocols retain
CPython execution. A definition inside an unsupported enclosing function belongs
to that function's fallback boundary. Analysis never executes these definitions.
This is an explicit definition boundary, not a failed lowering repaired by skipping
statements or repeatedly executing expressions.

Unsupported module-level statements and future compiler flags use whole-module
fallback: the original source is compiled and evaluated in the actual module
dictionary. Native and mixed code still use CPython; “native” describes lowering,
not a standalone runtime. Fallback is not acceleration. The CLI allows reported
compatibility execution by default, with `--no-fallback` rejecting every boundary.
The API preserves the earlier policy: projects require `fallback=True`; isolated
scripts allow compatibility execution unless disabled. Local project imports are independently compiled and linked
through the registry; original source files are not required at execution time.
External dependencies and data resources are not bundled.

Script `__file__`/`sys.argv[0]` use the absolute source path, and its original
parent directory is placed first on the runtime search path. The working directory
is inherited. Interpreter path initialization uses the selected Python executable,
including its virtualenv and site-packages, instead of guessing a Python prefix
from the generated binary location. That Python installation must remain available. Metadata retains original source paths, but converted modules execute from C
without those files. External resources and runtime dependencies still need deployment. Python executable/loader/import-path implementation
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

O0 emits required lowering. O1 removes unreachable blocks with verifier checks.
O2 additionally rewrites add/subtract/multiply to checked exact-integer fast paths:
`PyLong_CheckExact` guards each operand, `PyLong_AsLongAndOverflow` checks conversion,
and compiler overflow intrinsics check the machine operation. Any type/range miss
uses the original CPython numeric API; bools and subclasses keep generic dispatch.
Those individual operations still box results. Eligible pure-integer functions also
receive a validated scalar kernel IR: exact integer arguments, checked long
arithmetic, native locals, range/while loops and guarded self-recursion. They box
at the public return, retaining original parameter/constant objects where needed
for return identity. Range and recursive callable bindings are guarded. Overflow,
uninitialized locals, unsupported argument types and iteration budgets decline
to the original generic CFG. Replay is limited to functions proven free of heap
writes and unknown calls. The budget is 100 million loop iterations per kernel
invocation. Floats, arbitrary function specialization/inlining and resumable
mid-function deoptimization are not implemented. Native frame/tracing exclusions
still apply; kernels hold the GIL and do not provide CPython bytecode scheduling.
O3 also folds branches on immutable literal constants, inserting the required
register release before removing unreachable blocks. Assertions remain active at
all levels. Passes run on native portions of mixed modules and report decisions.
Generic arithmetic and guarded slow paths use CPython APIs and can incur overhead. No performance
improvement is promised. Generated sources are deterministic for identical source
locations, target/configuration and input bytes; native binaries depend on the
host toolchain and are not claimed reproducible across hosts.

## Commands and results

Implemented commands: `transpile`, `build`, `run`, `check`, `inspect`, `benchmark`,
`info`, `--version` and `--help`. Script arguments follow `--` for run/benchmark.
Use each command's `--help` for implemented options. Package/directory and explicit module entries, project roots and additional source
roots are implemented; see [Packages](PACKAGES.md). Interactive initialization
is not implemented.

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
filesystem effects or measure peak RSS. `--function NAME` instead loads a file as
an extension and times repeated calls after imports. It compares type-tagged
builtin results, exception classes/messages, stdout/stderr and private working
directory file hashes. Fixture copying, result validation and imports are outside
timing. It reports raw samples, medians, IQR and whole-child peak RSS (including
validation overhead). Absolute-path/external effects are not sandboxed; function
results must be supported builtin values. Dynamic fallback instruction percentages
remain unimplemented. See [Benchmarks](BENCHMARKS.md).

## Validation and remaining work

The regression suite compiles real generated C at both implemented levels with
GCC/Clang when available, compares with CPython, checks error classes, native and
fallback extension loading, repeated-call reference counts, arbitrary precision,
constant identity, source encoding, imports/argv, output protection, IR rejection,
CLI behavior, benchmark mismatch handling and a seeded arithmetic corpus.
A local UBSan smoke run checks generated factorial code; this is not a comprehensive
sanitizer campaign. Wheel verification checks bundled runtime C/header files and
an installed-package smoke build outside the checkout.

CIR verification additionally checks program modes, function symbols/signatures,
default layouts, arithmetic operation domains and tuple access bounds from verified
unpack operations. CIR schema is version 2 and runtime ABI is version 3.

Next milestones are more complete exception/lifetime/finalizer tests, function wrapper
metadata, broader numeric specialization, additional optimization passes,
and additional platform validation. Relocatable Linux CPython bundles are available
through `Calcium.bundle` / `--bundle`; see [Deployment](DEPLOYMENT.md).
The longer-term [Roadmap](ROADMAP.md) remains the direction rather than a claim
that every phase has been completed. Magnesium/Iodine are not required: their
compatibility issue remains documented in [Architecture](ARCHITECTURE.md).
