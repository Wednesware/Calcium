# Bonemarrow native backend — 26.7

Calcium is the public CLI and API; **Bonemarrow (`bmrw`)** is the independent
Python-to-C execution engine, using CaRT. Bonemarrow is the default in 26.7.
`--use bmrw`, `--bmrw`, and legacy `--backend native` select the same engine.
`--cpython` / `--use cpython` / `--backend cpython` explicitly select the
established compatibility backend. Conflicting selections are errors.

```sh
ca main.py -O3
ca package/ --run
ca --module package.cli --project-root ./src --keep-c
ca program.py --transpile -o build/c
ca program.py -v --fancy
ca existing_application/ --cpython
ca --info
```

The Python API defaults to `Calcium(backend="bmrw")`; `backend="native"` is an
alias, internally normalized to `native` for existing result/manifest consumers.
Both names always reject unsupported features, regardless of `fallback=True` or
`--allow-fallback`. Use `Calcium(backend="cpython", fallback=True)` for broad
Python compatibility. Unsupported native features are never delegated to CPython.

The compiler runs on CPython 3.12–3.14. Its generated executables do not query a
Python SDK, include Python.h, link libpython, initialize an interpreter, embed
Python source or bytecode for execution, or spawn Python. Builds require GCC or
Clang and Linux LP64/IEEE binary64. Executables need compatible platform C/math
libraries and an ELF loader; this is not static-libc/cross-platform support.
CPython extensions and `--bundle` require explicitly selecting `--cpython`.

## Projects and imports

Script inputs follow statically discovered project imports. Package/directory
inputs use the existing directory discovery rules (including other local `.py`
files). A package executable needs `__main__.py`; plain directories need
`main.py` or `__main__.py`. `--module package` and `--module package.cli` implement
module entry points. `--project-root`, repeated `--include-path`, and
`--include-all` reuse the shared filesystem-only resolver, never execute build
hooks, and preserve source-root/symlink checks.

Supported imports include absolute/dotted/relative imports, aliases, from-imports,
package initializers, regular/nested/namespace packages, and circular dependencies.
Import statements inside functions and control flow initialize dependencies when
executed, rather than in discovery order. There is one C translation unit per
compiled module, with unique hashed external module symbols and private function,
constant, and global symbols. `program.c` links their registry and starts the entry.
`--keep-c`, `--emit-ir`, `--emit-ast`, and `--transpile` retain inspectable output.

Each CaRT module has its own namespace and initialization function. States are
absent, initializing, and loaded. Initialization runs once; circular imports see
partial namespaces and missing names raise ImportError/AttributeError. A child is
bound onto its parent only after successful initialization. Failed modules reset
rather than being recorded as loaded. Normal imports of the executable source
have a distinct namespace from `__main__`, matching script/module execution.
Cross-module calls dispatch to compiled CaRT functions without Python objects.
`__name__`, `__package__`, and `__file__` are available; packages expose `__path__`.
Source-location strings are metadata, not a runtime requirement for those files.

Only declared Python source roots are compiled. Standard-library imports without
native implementations receive located C160 diagnostics with `--cpython` guidance.
Other unresolved names remain explicit in the analysis/manifest and raise
ModuleNotFoundError if executed; missing from-import names raise ImportError.
No host importlib lookup occurs at native runtime. Dynamic imports, star imports,
custom import hooks, module `__getattr__`, mutable search paths, `__spec__`/
`__cached__`/`__builtins__` introspection, and CPython extension loading are not
implemented. Adding files with `--include-all` does not implement dynamic imports.

## Language support

| Feature | Bonemarrow 26.7 |
| --- | --- |
| Values | None, bool, arbitrary-precision int, IEEE double, UTF-8 str, functions, ranges, modules, lists, tuples, dictionaries, sets |
| Arithmetic | `+`, `-`, `*`, unary `+`/`-`; checked integer overflow promotes to big integers, mixed int/float comparison is exact |
| Strings | Concatenation, repetition, truth/comparison, code-point indexing/slicing/iteration; UTF-8 stdout; collection repr uses Unicode 16 printability tables |
| Collections | Literals/expansion, constructors, iteration, length, indexing, sequence slicing, list/dictionary item assignment, membership, comparisons, list concatenation/repetition/in-place mutation |
| Methods | list append/extend/pop/clear/copy, dict get/update (dictionary source)/clear/copy, set add/clear/copy; saved bound methods keep their receivers alive |
| Functions | Undecorated top-level definitions, aliases, recursion, positional-only/keyword-only/default/variadic parameters, named/expanded calls; mutable defaults evaluate at definition time |
| Annotations | Unshadowed builtin int/float/bool/str or None on signatures; never trusted as unchecked optimization proof; reflection unsupported |
| Control flow | if/else, conditional expressions, boolean/chained comparisons, while/for with else/break/continue, global declarations, ordinary nested unpacking assignment |
| Builtins | print (sep/end/flush/file=None), range, len, list, tuple, dict, set |
| Identity | Only explicit comparisons with None or bool are accepted; arbitrary identity is rejected |
| Errors | Native error class/message and nonzero exit; deterministic frame unwinding, no Python tracebacks or catchable structured exceptions yet |

Dictionary/set storage is currently a contiguous, insertion-preserving value
array with **linear lookup**, not a native hash-table optimization. Dictionary
replacement preserves the original key object and insertion position. Sets
preserve membership/equality/subset semantics; their unspecified iteration/repr
order can differ from a particular CPython build/hash seed. Container/string
operations are generic CaRT operations, not unboxed container specializations.
`dict()` accepts a dictionary, iterable pairs and named values; `dict.update`
currently accepts a dictionary positional source, without keyword expansion.
Dictionary views and other unlisted methods are not implemented.

The runtime is single-threaded, with 256 active calls and 128 parameters/call
arguments (including expanded calls). Source list/tuple/set literals have at most
128 elements, dictionaries 64 entries; runtime containers can grow beyond these
frontend limits. Large CIR frames use checked heap allocations. Basic signatures
may shadow builtin parameter names; some global builtin shadowing is conservatively
rejected. Embedded-NUL keyword names in expanded calls are unsupported.

Still rejected or unimplemented: classes/instances/custom attributes/descriptors,
closures/nested or conditional definitions/decorators, comprehensions, generators/
async, structured exceptions/raise/assert/with, bytes, slice assignment, starred
unpacking assignment, deletion, f-strings, walrus, division/floor division/modulo/
powers/bitwise operators, arbitrary annotations/future flags, surrogate string
code points, standard-library/native-library bindings, and arbitrary reflection.
Unsupported syntax gets C160; unsupported dynamic operations/methods produce
native errors. No compatibility fallback occurs in either case.

Exact error wording, object addresses, general numeric identity, GC scheduling,
resource exhaustion, CPython-specific I/O/digit-limit environment configuration,
thread/signals/finalizers/weakrefs are outside this milestone's contract.

## CaRT ABI 2 and ownership

CaRT's internal ABI is version 2, not a stable public C ABI. `CaValue` directly
tags None/bool/int64/double. Big integers use signed base-2^30 limbs, checked
promotion, and CaRT decimal conversion; printing retains the default 4300-digit
limit. Checked O2/O3 scalar integer kernels and guarded same-module direct calls
from 26.6 remain available. Cross-module functions use native tagged calling
conventions, with no Python tuple/dictionary argument allocation; variadic Python
parameters necessarily materialize native tuple/dictionary values. Cross-module
scalar specialization, general inlining and float kernels remain future work.

Tagged floats carry a copy-preserved identity token, so NaN membership/key/container
equality agrees with Python without heap-boxing numeric values. Primitive `nan == nan`
remains false. This does not expose general `id()` or numeric `is` semantics.

A non-moving mark/sweep heap owns strings, big integers, ranges, iterators,
containers, slices and bound methods. Register/local frames, module globals,
constant/default pools and extra module attributes are roots. Container edges,
method receivers and iterators are traced. **Iterative marking** terminates on
cycles without recursing through the C stack. Resizable container buffers are
included in heap-byte accounting and freed during sweeping/shutdown.

Collection occurs at generated basic-block safepoints and explicitly rooted
runtime iteration loops. C intermediates in other operations are not collected
mid-operation. Global/default roots are registered before initialization, so
cyclic imports and early allocations are safe. Shutdown frees module namespaces,
container buffers and the heap; generated functions unwind their frames on errors.
No user finalizers or weakrefs are exposed.

## Builds, logging and remaining milestones

Both backends reuse the validated content-addressed object cache, with distinct
native ABI/backend identities. Native cache keys cover preprocessed C/header
contents, compiler binary/version, flags and runtime ABI, without a Python SDK.
Every build relinks. Debug and `--no-cache` builds bypass caching; `--cache-dir`
selects a location. Corrupt/unavailable caches safely miss. Compiler warnings,
commands and cache outcomes are available in result metadata.

Normal builds print only artifact paths. `-v`/`--verbose` reports selected engine,
actual O-level, compilation/cache reuse and linking to stderr. `--fancy` explicitly
adds the awakening message. JSON stdout stays machine-readable; generated
executables print no compiler/startup identity messages.

Stages A–C (projects, module runtime, static import semantics) and E are shipped.
Stage D is partial: collections, indexing/slicing, richer signatures/calls and
common methods are implemented; classes, structured exceptions, closures and
library bindings remain open. Stage F includes differential, source-free,
ASan/UBSan, wheel and three-runtime package benchmark checks. See
[NATIVE_BENCHMARKS.md](NATIVE_BENCHMARKS.md) and
[the Discribe blocker report](benchmarks/discribe-26.7-blockers.json).
**Discribe is not natively compatible in 26.7.** Its local checkout was analyzed,
not executed, and requires further language/library/GUI support.
