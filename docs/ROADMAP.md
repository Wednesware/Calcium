# Roadmap and architecture review

Status: implementation began following the user’s explicit authorization on
2026-10-23: “Please generate Calcium itself now.” See [current implementation
status](IMPLEMENTATION.md) for delivered scope; later milestones remain planned.
The milestone numbers describe dependency order, not deadlines or speed promises.
See [Architecture](ARCHITECTURE.md) for D01–D09 and risks.

## Implementation authorization

The user explicitly requested implementation after receiving the architecture PR.
That instruction authorizes starting production work without another approval
round. The prototype follows the proposed pipeline/CPython/CIR direction, with
scope documented in [Implementation](IMPLEMENTATION.md). CPython 3.14 is the local
validation target because it is installed with development headers; CI covers
3.12–3.14. Phase 1 is substantially implemented and some Phase 2/future target
capabilities use reported CPython fallback. Recursive project/package compilation and extensions shipped in 26.2. Version
26.3 adds path-first CLI, fuller native syntax/signatures, definition-level
fallback, checked integer operations at O2, literal branch simplification at O3,
and native object caching. Version 26.4 adds checked integer kernels, native
structured exceptions/comprehensions/context managers, separate compute benchmarks
and Linux CPython bundles. Broader specialization and later phases remain open.
MIT licensing was selected by the maintainer for 26.3. See [Status](STATUS.md).

## Original design decision register

| Item | Proposed resolution | Status / reviewer / date |
| --- | --- | --- |
| D01 | Embedded, GIL-enabled CPython; public version-specific API | Implementation direction authorized; long-term details proposed |
| D02 | Python 3.12/Linux x86-64/GCC+Clang/C11 first | Implementation direction authorized; long-term details proposed |
| D03 | Hybrid CIR: SSA temporaries, explicit slots/effects/error edges | Implementation direction authorized; long-term details proposed |
| D04 | Identical compatibility contract at all O-levels; guarded native types | Implementation direction authorized; long-term details proposed |
| D05 | Reported whole-function/module fallback, optional rejection | Implementation direction authorized; long-term details proposed |
| D06 | Executable first, extension later; generic shared ABI deferred | Implementation direction authorized; long-term details proposed |
| D07 | Optional Magnesium, Iodine deferred pending version compatibility | Implementation direction authorized; long-term details proposed |
| D08 | Explicit native introspection exclusions and fallback boundary | Implementation direction authorized; long-term details proposed |
| D09 | Maintainer-selected open-source license | Resolved: MIT, explicitly selected for 26.3 |

The original review checklist asked for explicit review of D01–D08 and the
Phase 1 subset below. Subsequent user authorization to generate Calcium superseded
that pause; further material compatibility changes should remain explicit.
A PR creation, automated check or absence of objections does not itself grant
approval. MIT was explicitly selected by the maintainer; dependency release validation is
not inferred from other Wednesware projects.

## Phase 0 — Architecture and feasibility

Deliverables: [Architecture](ARCHITECTURE.md), [IR](IR.md),
[Type System](TYPE_SYSTEM.md), [Runtime](RUNTIME.md),
[Optimizations](OPTIMIZATIONS.md), [CLI/API](CLI.md),
[Compatibility](COMPATIBILITY.md), [Testing](TESTING.md), this roadmap and
[README](../README.md).

Exit criteria: maintainers review the pipeline and CIR choice, numeric correctness,
reference/exception model, compatibility exclusions, targets and public API; resolve
D01–D08; accept staged limits. Actual Magnesium/Iodine interfaces and blockers are
documented, not assumed. Production code begins only after this gate.

Open feasibility questions to resolve through approved experiments: cost of object
API baseline, unboxed-region performance potential, practical Python-function
wrapper metadata, native traceback requirements and project import-loader behavior.
None requires promising complete Python compatibility in Phase 1.

## Phase 1 — Minimal vertical compiler

Dependency: Phase 0 approval. Establish packaging metadata and an explicit supported
host/target check, then loader/parser/scopes, minimal CIR/validator, C emitter and
small held-GIL runtime. Bringing basic boxed CPython operations forward into this
phase avoids inventing a temporary integer language with different semantics.

Proposed subset: one script; constants `None`, bool, int, float, str; local/module
bindings; expression statements; `+`, `-`, `*` through Python object APIs; comparisons;
`if`, `while`, `return`; undecorated functions with fixed positional arguments and
no defaults/closures; calls including a dynamically resolved builtin `print`.
Validate reads-before-binding, recursion and basic error propagation. All other
constructs receive a located unsupported-feature diagnostic. No fake fallback.
No annotations are evaluated during analysis or treated as runtime guarantees.

Deliver initial `check`, `inspect`, `transpile`, `build`, `run`, `info`, Python API
and O0 for implemented features. Build toolchain remains separate from emission.

Exit criteria:

- End-to-end programs with functions, branches and loops match CPython output,
  return status and exception type; huge integers remain arbitrary precision.
- CIR validator rejects missing error edges, invalid types and ownership bugs.
- Generated C compiles with both GCC and Clang against the declared Python SDK.
- C output does not require a C compiler to generate; analysis executes no sources.
- Unsupported cases, absent compilers/headers and invalid options fail actionably.
- Repeated emission yields identical source/manifests aside from explicit timings.

## Phase 2 — Runtime correctness and fallback

Dependency: Phase 1's object/error/ownership contracts. Add full argument binding,
defaults and closures, containers, dynamic attributes, exception handling, imports
and initial class behavior. Introduce original-code whole-function/module fallback
before accepting complex syntax beyond native lowering. Keep native introspection
limitations prominent. External imports use CPython without assuming packaging.

Exit criteria: differential suites for mutation, closures, mutable defaults,
operator overloading, exception cleanup, GIL/thread-state use and runtime import
behavior. Debug builds and available sanitizers find no Calcium ownership/UB
errors. Unsupported advanced native constructs execute only at reported fallback
boundaries, with no partial-source `exec` substitution or repeated side effects.

## Phase 3 — Inference and safe optimization

Dependency: stable generic semantics and differential harness. Implement flow-sensitive
facts, exact-type/range guards, checked arithmetic, O1 passes, then O2/O3 features
as each passes its gates. Add bounded specialization and optimization/fallback
reports. Prefer guarded numeric regions with demonstrated benefit.

Exit criteria: adversarial arithmetic/subclass/incorrect-annotation/alias cases pass
at every enabled level; property fuzz corpus has no unresolved semantic mismatch;
IR invariants hold after every pass. Publish raw benchmark measurements only after
correctness checks. Demonstrate at least one repeatable workload benefit without
implying all programs improve; document startup and fallback regressions.

## Phase 4 — Projects and extension artifacts

Dependency: runtime imports from Phase 2; module executor/state and toolchain ABI
contracts. Add explicit project roots, qualified-module manifests, native
finder/loader, package initialization, multi-unit linking and extension target.
Add content-addressed build caching only after deterministic manifests stabilize.
Do not couple this phase to completing every advanced O3 pass.

Exit criteria: relative/circular/conditional imports, initialization failure/retry,
import hooks and `sys.modules` behavior match CPython. Extension initializes in
the existing host and never finalizes it; state is per module. Artifact names and
ABI checks match the selected interpreter. Cache invalidates on sources, compiler,
Python ABI, runtime, flags or dependency changes; partial builds cannot poison it.
General `--target shared` remains an explicit unsupported feature pending ABI review.

## Phase 5 — Advanced native compatibility

Dependency: validated fallback baseline, resumable CIR and lifetime machinery.
Incrementally lower generators, coroutines, context cleanup across suspension,
advanced class protocols, pattern matching, exception groups and more extension
interactions. Keep each native feature behind its own acceptance evidence.

Exit criteria: differential cases cover send/throw/close, cancellation, async
finalizers, nested exception state and protocol dispatch. Any new introspection
claims require explicit frame/code-object/tracing tests. Unknown cases retain
fallback or reject; feature parsing does not imply native implementation.

## Phase 6 — Stability, performance and releases

Dependency: a documented tested subset, not completion of all Python features.
Expand fuzzing, profile hot runtime paths, run benchmark categories and establish
release/versioning procedures. Add platforms/Python minors only after their
compatibility matrix is green. Consider limited API or free-threaded support as
separate proposals when benefits justify their cost.

Exit criteria: approved license, installation docs, reproducible examples, tested
version/platform matrix, source-map/debug guide, supported-feature list, known
limitations and raw performance data. No unresolved correctness or native safety
regressions in the supported subset. Publish clear fallback coverage and required
CPython/dependency footprint.

## Separate future proposals

Standalone runtime, cross compilation, stable general-purpose C ABI, complete
frame emulation, subinterpreters and free-threaded execution each need explicit
architecture/ownership/testing decisions. Kefir integration, if ever useful,
should consume documented Calcium artifacts without a circular dependency.
These are neither hidden Phase 1 obligations nor promised capabilities.
