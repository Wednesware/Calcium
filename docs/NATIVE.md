# Standalone native backend — 26.6

26.6 delivers the first vertical slice from [26.6_PLAN.md](../26.6_PLAN.md).
CPython remains the default. Native mode is an independent execution backend;
unsupported input is rejected, even when `--allow-fallback` is present.

```sh
ca build examples/native/pass.py --backend native -o build/pass
ca examples/native/hello.py --backend native --run
ca examples/native/sum_squares.py --backend native -O3 --run --keep-c
ca check program.py --backend native --optimization-report
ca --info --backend native
```

The Python compiler/frontend still runs on CPython 3.12–3.14. Native C emission
and compilation do not query a target Python executable, use Python headers,
link libpython, embed Python source/bytecode, or invoke Python in the generated
executable. A C11 compiler and the target platform C/math libraries suffice.
Initial validation covers Linux LP64 (64-bit `long`, 8-bit bytes) and IEEE binary64.
This is not a static-libc/cross-compilation or CPython-extension ABI claim.

`Calcium(backend="native", target="executable", optimization=3).build("program.py")`
selects the same backend in the API. Default `backend="cpython"` retains existing
packages, imports, extensions, caching, fallback and CPython bundle behavior.
`--backend native --target c` / `--transpile` emits inspectable C, the CaRT header
and sources, and a manifest. `--keep-c`, `--emit-ir` and `--emit-ast` retain build
intermediates. Native object caching is not implemented. `--python` selects the
compatibility interpreter for the CPython backend; native compilation does not
use it. Native executables already contain CaRT and reject `--bundle` and
`--target extension` rather than silently switching runtimes.

## Supported source subset

| Feature | Native 26.6 behavior |
| --- | --- |
| Values | None, bool, arbitrary-precision int, IEEE float, UTF-8 str, compiled functions, ranges |
| Arithmetic | `+`, `-`, `*`, unary `+`/`-`, with checked machine integers and big-integer promotion; mixed int/float conversions round to even and report overflow |
| Strings | Literals, concatenation, integer repetition, truth, lexicographic/equality comparisons, Unicode-code-point iteration and printing |
| Comparison/conditions | Numeric comparisons, exact mixed int/float comparison, chained comparisons, `and`/`or`/`not`, conditional expressions and if/else |
| Variables | Local/global bindings, augmented name assignment and global declarations |
| Loops | while/else, for/else over ranges and strings, break/continue; arbitrary-precision range bounds/steps |
| Functions | Undecorated module-level definitions, positional/positional-only and named arguments, aliases, recursion and return |
| Annotations | Only unshadowed builtin int/float/bool/str and None on basic function signatures; never used as unchecked type guarantees; annotation reflection is unsupported |
| Builtins | print (sep/end/flush, file=None) and range |
| Errors | Native error indicator with exception class/message, deterministic unwinding and nonzero exit; no Python traceback/frame emulation |

Native print writes UTF-8 and does not emulate Python-specific I/O environment
configuration. Native execution is single-threaded. Recursion is capped at 256 active calls,
with `RecursionError` instead of allowing C-stack exhaustion. Calls and signatures
are limited to 128 arguments/parameters. Large CIR frames use checked heap storage;
small frames and primitives use the C stack/tagged values. Function definitions
inside control flow and nested definitions are rejected. Only basic name stores
are implemented. Builtins other than print/range, implicit module metadata and
reflection are unsupported; local parameter names may shadow builtin names.

Unsupported: imports/packages/standard-library modules, bytes, lists, tuples,
dicts, sets, indexing/slicing, comprehensions, defaults, keyword-only/variadic or
expanded signatures/calls, classes/instances, attributes/methods, inheritance,
closures, decorators, descriptors, generators/async, exceptions/handlers,
identity/membership tests, division/floor division/modulo/powers/bitwise operators,
f-strings, arbitrary annotations, future imports and surrogate string code points.
They receive located `C160` diagnostics, not CPython execution. Modules/packages
are rejected as unsupported native configuration. Third-party CPython extensions
cannot be loaded. Use the CPython backend when these capabilities are needed.

Error classes for supported runtime errors are tested against CPython. Exact
argument-error wording, tracebacks, introspection, object addresses/identity,
GC scheduling, signal/thread scheduling and resource-exhaustion behavior are not
claimed identical. No unsafe annotation/overflow opt-in changes Python arithmetic.
Future object-model support must resolve identity/materialization before exposing
`is`, `id`, finalizers or weak references.

## CaRT ABI 1 and ownership

`runtime/native/include/cart.h` declares an internal runtime ABI, not a stable
public general C ABI. Implementation is split across values/function/range logic,
integers, float formatting and memory management in `runtime/native/src`.

A `CaValue` tags None/bool/int64/double directly; these operations need no numeric
heap object. Integers beyond int64 use signed base-2^30 limbs. Add/subtract/multiply
check overflow before C signed arithmetic and promote to arbitrary precision.
Decimal conversion is implemented in CaRT. Float formatting finds a shortest
round-trip decimal using libc correctly rounded formatting, then applies Python's
fixed/scientific thresholds; IEEE-pattern tests compare against CPython.

Immutable strings, large integers, ranges and internal iterators live in a
non-moving mark/sweep heap. Generated function/register/local slots, globals and
constant pools are explicit tracing roots. Collection occurs only at generated
basic-block safepoints, never halfway through a runtime operation with unrooted
C intermediates. Return values transfer to caller slots before the next safepoint.
All frames unwind on failure; shutdown frees the heap. Memory-pressure thresholds
trigger collection and retain reachable/shared references. An internal cyclic
object-graph test verifies tracing termination and reclamation, but mutable Python
containers, user finalizers and weak references are not yet exposed.

This deliberately avoids premature reference-count/cycle-collector machinery.
Future mutable container/object tags need graph traversal and finalizer policies;
adding an enum name is not implementation of those Python types.

## Reused compiler infrastructure and specialization

The existing filesystem loader, parser/semantic compiler, scope allocator,
constant-folding decisions, CIR builder/ownership validator, CFG passes and typed
pure-integer kernel IR remain in use. A separate native capability gate rejects
unsupported constructs and normalizes only benign primitive signature annotations.
The C generator consumes CIR, not Python AST nodes.

O0 emits generic CaRT operations with tagged primitive fast paths. O1 removes
unreachable CIR blocks. O2/O3 enable checked scalar kernels for eligible pure
integer functions/range/while regions, guarded direct calls to compiled functions,
and direct scalar self-recursion. Overflow/type/binding/budget misses replay only
proven-pure kernels through generic CaRT, never CPython. O3 additionally uses the
existing immutable-literal branch simplification. GCC/Clang receive the matching
O-level with fast-math and FP contraction disabled. General inlining, cross-module
scalar specialization, escape analysis, allocation elimination and LTO are not
implemented.

Native optimization reports distinguish specialized/generic functions, guarded
native call sites, generic dispatch and potential heap-operation/frame sites.
Primitive numeric materialization uses a tag, not a heap box. Counts are static
sites, not runtime allocation/hotness measurements. Retained generic slow paths
can appear alongside a scalar kernel in generated C.

## CPython dependency inventory

The compatibility backend retains its dependencies intentionally:

- `backend/generator.py`, `backend/kernels.py` and `backend/project.py` generate
  `PyObject` wrappers, object operations, scalar guards and interpreter launchers.
- `runtime/calcium_runtime.h/.c` includes Python.h and implements PyNumber/object,
  call/binding and Py_InitializeFromConfig behavior.
- `runtime/calcium_modules.c` implements its importer through CPython/importlib.
- `backend/toolchain.py` queries the selected Python SDK and links libpython.
- `api.py` keeps this path behind the default CPython backend. Inference's
  `PyObject*` presentation is a compatibility-backend fact, not a native runtime
  dependency or an unchecked native type proof.

The new native path does not call these generators/runtime/toolchain queries.
Tests replace the Python SDK query with a failure and still emit native C, compile
with a nonexistent target Python path, delete source files and run with empty PATH
and invalid Python environment paths. Emitted C is checked for Python.h/PyObject/
Py_Initialize references, and `ldd` is checked for libpython.

## Benchmarks and next milestones

See [26.6 measurements](NATIVE_BENCHMARKS.md) for a three-runtime suite, raw wall/CPU/
RSS samples, binary sizes, compilation times and unsupported categories. Benchmarks
validate output/status/file effects before reporting performance; GUI responsiveness
is not inferred from startup. Existing real projects such as Discribe still need
the CPython backend.

Next work: general values/containers, defaults/conversions, structured exceptions,
object identity/classes/closures, native modules/import caching and standard-library
bindings, plus wider fuzzing and ownership/finalizer campaigns. CPython remains the
default until native compatibility/reliability warrant a separate decision.
