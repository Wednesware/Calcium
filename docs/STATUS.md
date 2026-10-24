# Remaining-work ledger — 26.3

This ledger distinguishes shipped work from architecture projects. A passing
regression suite establishes tested behavior; it does not establish complete
Python compatibility or every possible lifetime/finalizer behavior.

| Review item | Status after 26.3 |
| --- | --- |
| Simpler CLI | Delivered: `ca main.py`, `ca my_package/`, action options, legacy commands |
| Fixed-only native signatures | Delivered: defaults, positional-only/keyword-only, variadic args/kwargs |
| Missing native expressions | Delivered: chained comparisons, f-strings, walrus, call expansion |
| Assignment limits | Delivered: nested/starred unpacking; attribute/subscript augmented assignment; globals; deletion |
| Native assertion/raise | Delivered, including explicit causes and reraising the active exception |
| Whole-module fallback penalty | Reduced: complete function/class/async definitions have reported individual boundaries; enclosing native code remains compiled |
| Native exceptions/classes/closures | Still incomplete: handlers, context managers, classes, closures, decorators, generators, async and comprehensions use reported compatibility execution |
| Dynamic dict keys/set expansion | Still require fallback; full native construction-order coverage remains needed |
| O2/O3 unavailable | Delivered: checked integer operation fast paths and literal-branch simplification; general specialization/inlining/loop passes remain open |
| Unboxed numeric regions | Not delivered; fast-path values box at operation boundaries; no broad speedup claim |
| Incremental builds | Delivered: validated content-addressed native object caching; every build relinks |
| Folded NaN/import hooks | Fixed NaN emission, sign and constant identity; plain imports pass CPython-compatible fromlist values |
| Relocated Python build failure | Fixed: matching libpython can be resolved within selected interpreter prefix |
| Function metadata, frames, reflection | Native wrappers still differ; Python fallback definitions preserve ordinary Python function behavior; exact native frame emulation remains open |
| Memory lifetime | Shared constant-pool retention remains; more exhaustive exception/finalizer/weakref tests remain needed |
| Testing | Expanded differential, boundary, cache and sanitizer tests; comprehensive fuzzing/sanitizer campaigns remain open |
| Benchmark observability | stdout/stderr/status comparison remains; filesystem effects, peak RSS, kernel-only timing and runtime fallback percentages remain open |
| Deployment/resources | External packages/data and CPython deployment still required; standalone bundling remains open |
| Platforms/ABI | Linux and GIL-enabled CPython 3.12–3.14; Windows/macOS, cross compilation, free-threading, subinterpreters, standalone runtime and general C ABI remain open |
| Interactive project initialization | Not implemented |
| License | Resolved: MIT selected explicitly by maintainer; LICENSE included in distributions |
| Stale roadmap/CLI docs | Updated to reflect delivered package support and current CLI/optimization behavior |

Large remaining architecture projects need their own implementation and platform
validation. They are not represented as completed by fallback, packaging metadata,
a renamed option, or a higher optimization-level number.
