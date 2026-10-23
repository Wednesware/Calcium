# Python compatibility contract

Long-term compatibility proposal; see [Implementation](IMPLEMENTATION.md) for
current tested features and exclusions. A parsed
construct is not necessarily compilable, and CPython fallback is not evidence of
native coverage. First proposed target is conventional CPython 3.12 on Linux
x86-64; other minors/platforms require explicit validation.

## Compatibility categories

Every region is classified as native lowering, runtime object operation,
whole-function/module code-object fallback, or rejected. Reports include source
spans and reasons. Default mode may retain Python code objects for unsupported
features. `--no-fallback` prohibits those code-object boundaries, not the CPython
object API on which the hybrid runtime depends.

Preserve value results, side-effect and evaluation order, dynamic dispatch,
exceptions, arbitrary-precision integers, globals/cells, import initialization and
supported object identity/lifetime behavior. Do not equate equal numeric output
with full compatibility. Undefined C behavior is forbidden at every O-level.

Exact memory addresses, timing, GC scheduling and implementation-specific allocation
statistics are not portable equality targets. Nevertheless identity relationships,
weakref callbacks and explicit observable finalizer behavior remain correctness
concerns; optimizations cannot use those exclusions to justify arbitrary changes.
Resource exhaustion and allocation-failure injection need separate documented
expectations: a native region may allocate differently, but detected allocation
failures must propagate correctly and clean up safely.

## Planned progression, not support claims

| Feature | First treatment | Native expansion gate |
| --- | --- | --- |
| Literals, locals, expressions, if/while, basic functions | Phase 1 restricted lowering; reject the rest explicitly | Unit and differential cases |
| Integers, floats, booleans, text/bytes | Boxed CPython operations first | Exact guards, range/FP/identity tests |
| Lists, tuples, dicts, sets and indexing | CPython runtime operations in Phase 2 | Aliasing/mutation/error-order tests |
| Functions, defaults, keyword binding, closures | CPython objects/fallback until binding/lifetime model exists | Signature, shared cells and default mutation tests |
| Classes/inheritance/descriptors/metaclasses | Whole definition or module fallback | Class creation and dynamic protocol suite |
| Exceptions/with/finally | Rejection in restricted Phase 1; fallback in Phase 2 | Exception state/cleanup/re-raise tests |
| Imports, dynamic imports, external extensions | Runtime import machinery in Phase 2; project native integration in Phase 4 | Cycles, hooks, cache/failure rollback and metadata suite |
| Decorators/comprehensions/match | Parse and classify; whole-function/module fallback when available | Scope, ordering and protocol tests |
| Generators/async/exception groups | Whole-function/module fallback | Resumption, cancellation, cleanup and exception-group tests |
| `eval`/`exec`, `locals`, frames, tracing | Conservative enclosing fallback; some interactions rejected | Explicit frame/introspection implementation proposal |
| Native CPython extension modules | Runtime import with matching ABI/dependencies | Package-specific integration evidence |
| Free-threading/subinterpreters/standalone | Unsupported initially | Separate architecture review |

Until a functioning fallback runtime exists, unsupported constructs fail with a
source diagnostic. Never generate empty bodies, silent `None` values or discarded
operations. A module compiled entirely as a code object must be labeled fallback
execution, not an optimized transpilation success story.

## Fallback implementation boundary

Select fallback before execution. Compile original source using the matching
target interpreter at runtime, preserving filename, flags, module dictionary,
package metadata and builtins. A whole module fallback naturally preserves its
Python function definitions. Mixed-mode substitution is permitted only when the
native function and retained Python surroundings satisfy documented semantics;
otherwise expand the boundary to the entire module. Do not stitch arbitrary
source expressions into `exec` with synthetic locals.

Reflection can arise through callers or imported code, not only a visible call to
`locals()` in the compiled function. Static detection is incomplete. Consequently
native function object type, `__code__`, exact frame layout, tracing/profiling and
traceback formatting are initial exclusions of the native contract. Programs
requiring these features must retain ordinary Python functions/modules; document
this even when direct reflection is not found by analysis. A future strict
introspection mode could choose fallback-only execution, not claim unproven native
transparency. The default support contract requires maintainer approval (D08).

Annotations retain the chosen Python version's runtime behavior. Analysis does
not evaluate them, and specialization does not turn them into checks the original
program lacked. Future-annotations and later-version deferred-annotation semantics
need version-specific handling before claiming those targets.

## Import and package execution

Analysis reads files and records dependencies without executing them. Local
module graph edges are hints about potential runtime work. Circular imports are
legal; import execution order is driven by actual Python control flow.

For native modules, use a scoped finder/loader through CPython's import protocol,
with a manifest mapping qualified names to compiled module executors. Do not
prepopulate all modules at startup or bypass `sys.meta_path`, `sys.modules`,
`__import__` rebinding, parent-package initialization or earlier custom finders.
An explicit runtime bootstrap installs the finder at a documented position:
after earlier application finders and before the normal path finder, handling
only manifest entries whose origin matches the selected project root. A custom
finder that resolves the module first retains precedence. Collision/shadowing
cases require tests before package support ships.

Let importlib own module insertion and rollback around `exec_module`; partially
initialized modules remain visible during valid cycles, and failed module
initialization is removed according to import rules. Do not manually apply a
static topological execution order. Preserve `__spec__`, `__loader__`, `__package__`,
`__path__`, `__file__` and package attributes. The native executor initializes the
provided module dictionary rather than creating a competing module object.

Scripts execute as `__main__` with script-path search semantics; `--module`
execution uses module/package semantics and the correct `__spec__`. Dynamic imports,
namespace packages, zip imports and external modules remain interpreter-managed.
Initial package support covers declared filesystem roots only; other loader kinds
can resolve dynamically but are not automatically compiled or bundled. Runtime
modifications to import hooks or `sys.path` must take effect normally.

Retain original source alongside early compiled projects for dynamic fallback and
source inspection unless the manifest explicitly embeds equivalent source data.
An executable does not bundle third-party distributions, standard library,
package resources or shared-library dependencies. Report these requirements.
Packaging remains separate from Kefir.

Reference: [Python import system](https://docs.python.org/3.12/reference/import.html).

## Dynamic binding and evaluation

Load globals/builtins according to Python lookup rules; do not freeze `print`,
`range`, `int`, attributes or function objects merely because their initial
binding is familiar. Attribute access may call descriptors; arithmetic can invoke
reflected methods; a comparison can return a nonboolean object. Generic calls and
refcount release can execute arbitrary Python and invalidate assumptions.

Preserve left-to-right expression order, short circuiting, once-only defaults,
closure cell sharing, augmented assignment protocols, container insertion order
and exception timing. Native arithmetic requires the contracts in
[Type System](TYPE_SYSTEM.md). No optimization level removes asserts or docstrings
by default, and no level opts into bounded integers silently.

## Publishing compatibility

Each release must publish a tested feature/platform/version matrix and known
exceptions, label fallback versus native coverage and include reproductions for
limitations. Support for an external package is demonstrated on specific versions;
loading CPython does not justify a blanket ecosystem compatibility claim.
The current runtime validation boundary is documented in [Implementation](IMPLEMENTATION.md); full Python compatibility is not claimed.
