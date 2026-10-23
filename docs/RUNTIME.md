# Runtime and C backend

Status: proposed; decisions D01, D02 and D06 in [Architecture](ARCHITECTURE.md).
Initial artifacts require a matching CPython runtime and its standard library.
Generating an executable does not make its Python ecosystem dependencies vanish.

## Execution and lifetime

Executable entry points own initialization through `PyConfig` and
`Py_InitializeFromConfig`, configure `sys.argv`, script/module identity and search
paths, and clear configuration on every exit. Preserve normal environment/site
behavior by default; an isolated mode would need an explicit future option.
An embedded app executes one main module and finalizes only the interpreter it
created. Check finalization/stream flush failure in exit status policy. Handle
`SystemExit`, `KeyboardInterrupt`, signals and unhandled exceptions consistently
with the declared compatibility scope.

An extension initializes inside an existing interpreter; it never initializes or
finalizes the host. Use multi-phase module initialization and per-module state for
owned Python references. Do not store mutable Python objects in process-global C
statics. Initial support is one conventional GIL-enabled main interpreter;
subinterpreters, free-threaded builds and interpreter reinitialization are rejected
or marked unsupported until separately validated.

All object API operations require the appropriate attached thread state and GIL.
Native regions may release the GIL only after proving absence of Python API calls,
callbacks, reference changes and mutable Python state access. The first prototype
keeps it held. No worker thread may assume that a borrowed pointer remains alive
while another operation can invalidate its owner.

References: [Python initialization](https://docs.python.org/3.12/c-api/init_config.html)
and [thread/interpreter rules](https://docs.python.org/3.12/c-api/init.html).

## Ownership ABI

| Runtime value | Contract |
| --- | --- |
| Owned `PyObject *` | Caller must release once or explicitly transfer ownership |
| Borrowed `PyObject *` | Valid only while its witnessed owner/lifetime remains valid |
| Stolen parameter | Consumed under the helper's documented success and error rules |
| Native scalar | No Python ownership; boxing creates an owned reference |
| Null object result | Error sentinel; never Python `None` |

Calcium helpers use borrowed inputs and new owned object results by default.
Adapters normalize CPython APIs with unusual steal-on-error behavior; each adapter
has a written success/error consumption table. A naming convention alone is not
sufficient evidence of ownership. `retain`/`release` become the appropriate public
reference APIs. Each owned C local starts NULL; transfer clears the source local.

Backend emits cleanup labels derived from CIR liveness, not a blanket release of
all locals at function end. Binding replacement keeps correct references alive
while releasing prior values at the specified point. A `DECREF` may invoke user
code, mutate state or cause resurrection. Eliminating or moving it needs proof
that observable finalizer and weak-reference behavior is retained. Do not rely on
immortal objects or private object layouts.

Objects owned by Calcium's custom callable/closure wrappers participate in cyclic
GC with traverse/clear/deallocation support. Until these wrappers are implemented,
use CPython-managed functions/cells or reject the relevant native lowering. A
reference-count-only wrapper that leaks closure cycles is unacceptable.

Reference: [C API references and exceptions](https://docs.python.org/3.12/c-api/intro.html).

## Exception ABI and cleanup

Object helpers return NULL with an exception set on failure; status helpers return
`-1`. Nullable legitimate values use a separate status/out parameter. A guard miss
uses a distinct outcome from an actual exception. The IR must distinguish all
three. Never use nonlocal `longjmp` as generic Python exception propagation.

At each raising call, branch immediately on the sentinel. Error cleanup temporarily
owns the raised exception using public target-version APIs, releases temporaries
according to the validated cleanup plan, and restores the original exception.
Finalizer/unraisable errors follow CPython handling rather than replacing an
unrelated pending exception. An actual language `finally` clause differs from
reference cleanup: it can deliberately replace or suppress pending control flow.

Handled-exception state is tracked separately from a newly raised error indicator.
Support exception chaining, bare re-raise, context managers suppressing exceptions,
and nested handlers only after differential tests prove their state transitions.
C errors never leak stale error indicators onto a success return.

At executable boundaries render uncaught errors through Python's machinery.
`#line` improves C compiler diagnostics but does not create Python traceback frames.
Maintain a source-map sidecar and defer exact native frame/code-object emulation.
Functions requiring tracebacks/frames outside the supported contract use whole
function/module fallback. Do not promise identical native traceback formatting.

## Python functions and native kernels

All Python-visible calls first obey Python binding rules: positional/keyword-only
parameters, duplicate arguments, defaults, varargs, descriptors and exceptions.
Only a validated wrapper dispatches to a guarded private kernel. Python callers
never receive an unboxed C pointer as a function result.

Native kernels have explicit operand representations, required guards, GIL rules,
output ownership and error status. They return to a compatible object entry point.
CIR records every boxing boundary. A generic Python call uses public CPython call
APIs and respects the actual callable object, not its inferred original definition.
Reflection on native callable identity/signatures/code is an explicit limitation;
see [Compatibility](COMPATIBILITY.md).

## C output contract

Planned output directory for a script:

```text
build/main/
  main.c
  main.h
  calcium_runtime.h
  calcium_runtime.c
  source-map.json
  manifest.json
```

`transpile` produces all runtime sources needed by generated code; the native build
requires a compatible Python SDK. Runtime support is separate from generated module
code. Headers use include guards and runtime ABI version checks. Include `Python.h`
before other headers as required by its API. Emit C11, fixed-width types only where
available, and no user symbols in CPython's reserved namespace.

Symbol names combine a sanitized readable prefix with a stable digest of qualified
module/function identity and signature. Use sufficient digest length and detect
collisions. Escape source paths, identifiers, comments and byte strings safely;
never paste Python literals directly into C. Preserve original UTF-8 source spans
and filename mappings. Debug output adds comments and `#line` directives; JSON maps
remain the authoritative relationship to Python source.

Separate each module into translation units and link against one compatible runtime
support implementation. Sort artifacts and normalize logical paths. Generated
sources should be byte reproducible for identical inputs/settings; native binaries
are only reproducible under a pinned toolchain/environment. Do not promise it
across compiler versions. Manifests include Python ABI, platform, runtime-helper
version, source hashes, options and dependency requirements.

## Toolchain boundary

`transpile` never probes a C compiler. `build` resolves the explicitly selected
GCC/Clang executable, probes its version and queries the selected Python's
`sysconfig` through a controlled subprocess. The compiler host and target must match
the supported platform/minor/ABI. Missing headers, libpython, standard library or
unsupported configuration produce actionable diagnostics before native compilation.

Construct argv lists without a shell, with output/source paths separate from flags.
Do not execute project build hooks or install dependencies. Compile using a private
temporary directory and atomically publish completed artifacts. Do not overwrite
input files or partial previous artifacts. Link flags come from the selected
Python configuration; embedding and extension flags are different. Use strict FP
settings, appropriate optimization/debug flags and no unchecked signed overflow.
Toolchain exit codes, command argv and captured diagnostics appear in the result;
verbose output redacts unrelated environment secrets.

## Three different output ABIs

| Target | Initialization and entry | Initial commitment |
| --- | --- | --- |
| Executable | `main` owns CPython, executes module entry | First native target |
| Extension | Version-specific `PyInit_<name>` and module state; host owns interpreter | Later milestone |
| General shared library | Versioned C exports, opaque handles, explicit host/lifetime/error rules | Rejected until separate ABI design approved |

CLI `--target shared` is reserved for the third row and must fail clearly at first.
A `.so` extension must not be mislabeled a generic shared library. Proposed future
C ABI needs explicit interpreter ownership, thread attachment, UTF-8 conventions,
handle retention/release and error retrieval, with no borrowed raw `PyObject *`
escaping by accident. Standalone mode additionally requires independent numeric,
object, exception, import and GC runtimes and is outside the initial roadmap.
