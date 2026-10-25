# Remaining-work ledger — 26.6

This ledger distinguishes shipped work from architecture projects. A passing
regression suite establishes tested behavior; it does not establish complete
Python compatibility or every possible lifetime/finalizer behavior.

The existing rows describe the CPython backend unless stated otherwise. The native
backend is an initial standalone subset; see [its exact contract](NATIVE.md).

| Review item | Current status |
| --- | --- |
| Independent native backend | Delivered first CaRT/C11 milestone: primitives, big integers, strings, basic functions/loops, checked scalar kernels and guarded direct calls; no Python runtime fallback. Native containers/classes/imports/exceptions and ecosystem support remain open |
| Simpler CLI | Delivered: `ca main.py`, `ca my_package/`, action options, legacy commands |
| Fixed-only native signatures | Delivered: defaults, positional-only/keyword-only, variadic args/kwargs |
| Missing native expressions | Delivered: chained comparisons, f-strings, walrus, call expansion |
| Assignment limits | Delivered: nested/starred unpacking; attribute/subscript augmented assignment; globals; deletion |
| Native assertion/raise | Delivered, including explicit causes and reraising the active exception |
| Fallback visibility | Delivered: `--optimization-report`, qualified definitions and located categories, enclosing fallback boundaries, static counts, JSON/API/manifest output and partial-report marking; startup/runtime profiling remains open |
| Whole-module fallback penalty | Reduced: complete function/class/async definitions have reported individual boundaries; enclosing native code remains compiled |
| Native exceptions/classes/closures | Delivered: ordinary handlers, finally, synchronous context managers and list/set/dict comprehensions. Still incomplete: classes, closures, decorators, generators, async and except* use reported compatibility execution |
| Dynamic dict keys/set expansion | Delivered native dynamic keys and set expansion, with evaluation/hash-order differential checks; broader adversarial coverage remains useful |
| O2/O3 unavailable | Delivered: checked integer operation fast paths and literal-branch simplification; general specialization/inlining/loop passes remain open |
| Unboxed numeric regions | Delivered for pure integer functions: checked C locals/arithmetic/range/while and guarded direct self-recursion, boxing at public return. Floats, arbitrary-call specialization/inlining and mid-function deoptimization remain open; no broad speedup claim |
| Incremental builds | Delivered: validated content-addressed native object caching; every build relinks |
| Folded NaN/import hooks | Fixed NaN emission, sign and constant identity; plain imports pass CPython-compatible fromlist values |
| Relocated Python build failure | Fixed: matching libpython can be resolved within selected interpreter prefix |
| Function metadata, frames, reflection | Native wrappers still differ; Python fallback definitions preserve ordinary Python function behavior; exact native frame emulation remains open |
| Memory lifetime | Shared constant-pool retention remains; more exhaustive exception/finalizer/weakref tests remain needed |
| Testing | Expanded differential, boundary, cache and sanitizer tests; comprehensive fuzzing/sanitizer campaigns remain open |
| Benchmark observability | End-to-end output/status comparison plus function-only timings, typed results, exception/output/file-hash checks, raw samples/IQR and whole-child peak RSS. Runtime fallback percentages and end-to-end filesystem/RSS coverage remain open |
| Deployment/resources | Delivered relocatable Linux CPython folder bundle, selected environment dependencies, transitive non-system ELF libraries, package data and explicit script resources. Requires compatible OS C runtime/loader; arbitrary native package compatibility remains open |
| Platforms/ABI | Linux and GIL-enabled CPython 3.12–3.14; Windows/macOS, cross compilation, free-threading, subinterpreters, full native-runtime compatibility and general C ABI remain open |
| Interactive project initialization | Not implemented |
| License | Resolved: MIT selected explicitly by maintainer; LICENSE included in distributions |
| Stale roadmap/CLI docs | Updated to reflect delivered package support and current CLI/optimization behavior |

Large remaining architecture projects need their own implementation and platform
validation. They are not represented as completed by fallback, packaging metadata,
a renamed option, or a higher optimization-level number.
