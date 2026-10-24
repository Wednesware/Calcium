# Optimization framework

The full framework below is a design proposal. Delivered in 26.3: O0 baseline,
O1 unreachable blocks, O2 checked exact-integer add/subtract/multiply, O3 immutable
literal branch simplification, and corresponding GCC/Clang optimization flags.
Version 26.4 adds checked pure-integer scalar kernels, range/while regions
and guarded direct self-recursion at O2/O3. Other values remain boxed; general
specialization, CSE, inlining and loop transformations remain planned. See [Implementation](IMPLEMENTATION.md).

All levels preserve the same declared Python compatibility
contract. O3 is not permission to trust annotations or change arithmetic rules.
See [Type System](TYPE_SYSTEM.md) and [CIR](IR.md).

## Pass management

Each pass declares a stable name/version, required analyses, preconditions,
rewrite, preserved effects, ownership obligations, exception behavior and
invalidated facts. The manager validates input and output, recomputes invalidated
analyses and produces a deterministic decision log keyed to source spans/CIR IDs.
A pass can decline a rewrite with a reason; inability to prove safety is normal.

Report attempted, accepted and declined rewrites, guard/fallback locations and
code-size impact. Compile-time operation counts describe static coverage, not
runtime execution percentages. Dynamic counters are opt-in instrumentation and
must be labeled because instrumentation changes timing. Unexpected validation
failure is an internal error, never permission to emit the invalid result.

## Levels and staged enablement

| Level | Intended pass set | Initial prototype policy |
| --- | --- | --- |
| O0 | Required semantic lowering and ownership cleanup only | Reference baseline; debug-friendly output |
| O1 | Bounded exact-constant folding, immutable temporary propagation, unreachable block cleanup | Enable only individually validated passes |
| O2 | Dominance-aware simplification, pure native CSE, guarded numeric regions | Reserved until Phase 3 proof gates pass |
| O3 | Versioned specialization, limited inlining, loop and allocation work | Experimental candidates, not unconditional transformations |

The table describes the longer-term pass sets. O2/O3 are now available with the
narrower delivered behavior documented above; they do not imply all proposed
passes are implemented. In a later release a level may conservatively do no
additional work on a particular program; report that accurately. C compiler flags
are separate metadata, proposed `-O0`, `-O1`, `-O2`, `-O3` respectively, with strict
semantic flags overriding unsafe toolchain defaults. Calcium O-levels never imply
CPython's `-O` assertion removal.

## Pass safety contracts

| Pass | Preconditions | Exclusions / observable behavior |
| --- | --- | --- |
| Constant folding | Exact immutable operands; bounded computation; result semantics match target | No arbitrary calls or overloaded operators; preserve raising expressions and object identity obligations |
| Constant propagation | SSA value or stable slot with no intervening write/escape | Globals/cells/heap contents invalidated by unknown calls |
| Dead-code elimination | Unused result and operation proven effect-free/nonraising | Allocation, reference release, attribute reads and calls are not inherently removable |
| CSE | Dominating identical pure native expression with same inputs | No generic Python arithmetic or descriptor calls; preserve errors/FP rules |
| CFG simplification | Condition proven without observable truth conversion | Do not erase required `__bool__`, cleanup or handler behavior |
| Type specialization | Exact guards, range proof and generic slow path | Annotations/profiles alone insufficient; preserve subclass dispatch |
| Loop transforms | Known iteration, no observable mutation/callback/error-order change | No speculative hoisting of throwing operations or changed iterator semantics |
| Inlining | Actual callable identity, binding/default/closure dependencies proven | Preserve argument order, recursion limits contract, frames and exceptions; otherwise decline |
| Escape/allocation elimination | No identity, finalizer, weakref, alias or introspection observation | Cannot change required materialization or publish an unboxed value |
| Refcount elimination | Ownership proof and identical observable release point | No delay of finalizers or invalidated borrowed references |

Compile-time evaluation has budgets for integer bit size, sequence length, nesting
and operation count to avoid pathological input. Exceeding a budget declines
folding; it does not reject otherwise valid runtime code. Preserve runtime error
location/timing by leaving exceptional operations intact. Never import the
analyzed program to discover constants.

## Native arithmetic

Only explicit checked operations reach C. Integer overflow guards run before
signed operations that could overflow. Use generic Python dispatch for range/type
misses, preserve original operands and distinguish real errors from guard misses.
No `-ffast-math`, unsafe aliasing assumptions, unchecked shifts or division.
FP reassociation and fusion require proof against Python's observable results,
not simply a claim that the hardware supports them.

Native performance comes primarily from keeping validated numerical regions
unboxed and reducing dispatch, not merely translating each Python bytecode
operation into a CPython API call. Region boundaries box and check errors.
If preserving identity/lifetime requires original objects, retain them or decline
specialization. General state reconstruction/deoptimization is deferred.

## Acceptance and rollback

Every pass needs positive rewrite cases, negative semantic cases, pre/post CIR
validation, and differential execution against CPython at every supported level.
Native arithmetic additionally needs sanitizer and boundary tests. Performance
measurements follow correctness and include compilation/code-size costs.
A regression blocks that pass; disable it rather than narrow the compatibility
contract silently. Persist reproducing input and settings for fuzz failures.

A proposed future `--disable-pass NAME` debugging option may aid bisecting;
it is not part of the initial public CLI. Reports must distinguish runtime
fallback from optimization refusal and from a feature not yet implemented.
