# Validation status for 26.4

The suite now runs generated C at O0–O3, including checked-integer boundaries,
subclasses, expanded arguments, signatures/defaults, chained comparisons with
custom truth objects, native syntax additions, mixed definition fallback, and
object-cache reuse/invalidation/corruption. The Python matrix is 3.12–3.14.
`tests/test_sanitizers.py` adds opt-in UBSan runs for arithmetic and lifetime/error
paths. Version 26.4 passes 302 tests on each of CPython 3.12, 3.13 and 3.14,
with two opt-in UBSan tests run separately using GCC and Clang. Added coverage
includes checked scalar kernels and guard misses, native exception cleanup and
context-manager descriptor ordering, comprehension scope, compute benchmark
validation, and bundle relocation after removing sources/dependency environments.
Wheel/sdist and isolated installed-wheel smoke checks pass across the matrix.
The broader test strategy below remains a design, not a coverage claim.

# Correctness and performance validation

This is the long-term testing strategy. The implemented differential/native/CLI
suite lives in `tests/`; see [Implementation](IMPLEMENTATION.md) for the current
validation boundary. The additional campaigns below remain staged work.

## Stage tests

| Component | Required assertions |
| --- | --- |
| Loader | Encoding cookies/BOM, deterministic manifests, root/symlink policy, unresolved imports and legal cycles; no source execution |
| Parser/semantics | Syntax spans, unbound locals, globals/nonlocals, cells, comprehension scopes, future flags and target-version validation |
| Inference | Union joins, loop widening termination, alias invalidation, annotation nonenforcement, bool/int exactness |
| CIR | Dominance, terminators, effects, error edges, ownership, cleanup across joins; deliberately malformed IR rejected |
| Optimizer | Positive rewrites and adversarial refusals; verifier before/after every pass |
| Backend | C escaping, deterministic symbols, collision detection, source maps and valid runtime headers |
| Runtime | Ownership on success/error, exception state, default/call binding, GC cycles, interpreter lifecycle |
| Toolchain | argv/path handling, no shell, missing compiler/SDK diagnostics, atomic output and correct ABI flags |
| CLI/API | Applicable options, exit codes, valid JSON, no ANSI in machine output, no prompts/downloads in batch mode |

Use golden snapshots for stable generated C/CIR only where they express a public
or safety contract. Compile generated C as well; snapshots cannot detect all
invalid C, ABI misuse or undefined behavior. Avoid tests that merely mirror
implementation details without checking observable invariants.

## Differential execution harness

For every supported target, toolchain and optimization level:

1. Create separate temporary CPython/native working directories with identical
   fixture files, arguments, environment and import roots.
2. Run the original fixture using the exact selected target interpreter; record
   stdout/stderr bytes, exit/signal status, declared filesystem outputs and
   structured result observations emitted by the fixture.
3. Analyze, emit C and compile Calcium's version. Record stage time separately.
4. Run the artifact with matching runtime configuration and inputs.
5. Compare observations, then report a minimal mismatch with source, CIR,
   generated C, compiler argv, Python ABI and environment fingerprint.
6. Repeat with all implemented O-levels and both validated compilers.

A fixture can emit tagged JSON for return values, exception class/message/cause,
identity relationships and selected state. The harness does not serialize arbitrary
objects by `repr` and pretend that proves equivalence. Explicitly check bytes,
Unicode, dictionary order, return values and observable side effects. Exception
traceback format differs for native frames under the approved contract; compare
exception behavior separately from acknowledged presentation differences.

Normal program stdout/stderr and side effects are compared exactly by default.
Normalize only fixture-declared nondeterminism, such as a known temporary root or
address field; never remove arbitrary lines or exception text to make tests pass.
Use fixed seeds/time inputs where possible. Compare unordered values structurally
only when Python itself defines the relevant observation as unordered.
Timeouts distinguish nontermination and infrastructure failure from behavior
mismatch. Apply process/resource limits to fuzz cases; the compiler and harness
are not a security sandbox. Tests execute trusted fixtures in CI.

## Required adversarial corpus

- Integers around signed bounds, huge integers, shifts, negative floor division,
  zero division and bool/int subclass interactions.
- Incorrect annotations, union flow, redefined builtins, globals mutated by calls,
  dynamic attributes, reflected arithmetic and side-effecting comparisons.
- Closures with nonlocal rebinding, recursive calls, mutable defaults, decorator
  evaluation order, argument expansion/duplicate keywords and bound methods.
- Generators/async cancellation, comprehensions, context managers, nested handlers,
  `finally` overriding return/raise, exception groups and chained exceptions.
- Classes, inheritance, metaclasses, descriptors, weakrefs, resurrection and GC
  cycles; observable destruction order where the support contract specifies it.
- Circular/conditional/dynamic imports, failed initialization then retry, module
  caching, namespace packages, custom finders, shadowing and relative imports.
- Reflection/trace hooks verifying fallback classification; native exclusions must
  fail or report accurately rather than silently claim full support.
- Compatible extension packages, Unicode filenames, source encodings, paths with
  spaces/shell metacharacters, syntax errors and incomplete SDK configurations.

Unsupported features have negative tests for actionable diagnostics until their
fallback/native milestone arrives. Do not weaken a fixture when expanding support.

## Memory and native safety

Run generated programs under AddressSanitizer/UndefinedBehaviorSanitizer where the
selected CPython build supports them; document suppressions for interpreter noise.
Use debug CPython builds and reference/leak checks for the runtime. Inject failures
at Calcium allocation/helper boundaries to exercise error cleanup. Account for
CPython caches when interpreting retained allocations; check for unbounded growth
across repeated calls and cycles rather than requiring a naive zero count.

Arithmetic fuzzing must include overflow before C expression evaluation, not merely
correct output after UB already occurred. Verify guard-miss paths have no leaked
error indicators. Thread/interpreter tests are required before enabling any support
beyond the initial held-GIL single-interpreter contract.

## Property and fuzz testing

Use bounded generated programs from the supported subset, including randomized
value ranges and control flow. Optionally use Hypothesis as a development dependency,
not a runtime dependency. Store seeds, shrink failures and add each minimal
reproducer to the regression suite. Generate malformed CIR independently to test
validator rejection, and stress parser limits without importing analyzed sources.
A valid generated program outside the supported subset must reject cleanly.

## Performance methodology

Correctness validation precedes recording a speedup. Benchmarks are workloads,
not promised performance results. Categories: integer/float arithmetic, loops,
recursion, function calls, lists/dicts, strings, object creation, method dispatch,
numerical kernels, import-heavy startup and mixed native/dynamic operations.
Include fallback-heavy cases and regressions rather than cherry-picking wins.

Measure compilation stages, generated C bytes, artifact bytes and runtime dependency
footprint separately. For runtime, distinguish process startup/end-to-end latency
from steady-state computation. Use an in-program monotonic timer for kernel-only
runs with setup outside the measured region, identical workloads and consumed
results to prevent dead work elimination. Subprocess wall time includes startup
and must be labeled accordingly.

Use one warmup and at least seven measured repetitions by default, with larger
counts for noisy workloads. Alternate/randomize runner order, control affinity and
background load where practical, and record raw samples. Report medians, IQR (or
another explicitly named variability measure), repetitions and confidence method
if used. No blanket speedup assertions or invalid zero-time division:

```text
speedup = median CPython elapsed / median Calcium elapsed
```

For memory, measure per-process peak RSS using platform-specific tools and record
units/method; do not subtract unrelated process baselines silently. Fallback
percentage is opt-in dynamic instruction/helper instrumentation in separate runs;
static instruction coverage is reported separately. Record initialization overhead
using a minimal executable and explicitly labeled timing boundaries.

Every report includes CPU/OS, hardware topology, Python version/build/ABI, compiler
version/argv, runtime configuration, optimization level, source hashes, dependencies,
environment controls and raw data. Compare optional Nuitka/other compilers only
with equivalent workload/results and documented settings. Publication requires
an approved license, reproducible commands and known limitations.

## CI gates

Phase 0: document/link and diff checks only. Phase 1: unit and Linux integration
matrix, generated C compilation and differential subset at every implemented level.
Phase 2: runtime/ownership/error tests and debug/sanitizer jobs. Phase 3 onward:
optimizer fuzz regression corpus and guard-negative tests. Phase 4 adds import and
extension ABI fixtures. Cross-platform/minor-version jobs are admission gates for
those targets, not optional evidence after claiming support.

Keep performance jobs informational until stable hardware/baselines exist.
Correctness, native safety or schema regressions block release even if benchmarks
improve. Do not add a green placeholder CI badge that implies nonexistent compiler
validation.
