# Type inference and specialization

Status: proposed; decisions D02 and D04 in [Architecture](ARCHITECTURE.md).
Inference produces conservative facts, not an enforcement mechanism for Python
annotations. No analyzed expression or annotation is evaluated during compilation.

## Semantic lattice and representations

`Bottom` means no reachable values. `Unknown` is the top value set; `Any` from
annotations is tracked as a declaration without stronger evidence. Unbound state
is a separate slot property, allowing `UnboundLocalError` even after a known
assignment on some paths. `None` is a real value, not an unbound sentinel.

Semantic types include exact or subclass-capable `bool`, arbitrary-precision
`int`, `float`, `str`, `bytes`, `NoneType`, parameterized container shapes,
callables, classes, instances and finite unions. `Optional[T]` is `Union[T, None]`.
Track exactness explicitly: `bool` is an `int` subclass, and an `int` subclass can
override arithmetic. Builtin names can also be rebound.

Physical representations include `object`, `i64`, `f64` and native boolean.
A semantic Python `int` does not imply `i64`; an `object` reference can hold a
value whose exact type is known. Conversions are explicit CIR instructions.
Object identity and ownership remain obligations independent of numeric equality.

| Fact source | What it permits |
| --- | --- |
| Literal construction | Exact value/type at that point, with runtime allocation semantics retained |
| Annotation | Candidate ranking/reporting only; never unchecked lowering |
| Control-flow proof | Facts valid over dominated, unmodified values |
| Exact runtime type guard | Builtin operation rules for the guarded value |
| Range guard | Lossless conversion inside its success region |
| Profile observation | Specialization suggestion; never a permanent proof |
| Explicit bounded contract | Future opt-in ABI only; not part of initial Python-compatible compilation |

Evidence records origin span, guard block, assumptions, alias dependencies and
invalidation points. The optimizer consumes evidence IDs instead of guessing
from the displayed type name.

## Flow analysis

Analyze each function CFG to a fixed point. Assignments replace slot facts;
joins union possible types and widen intervals. After a bounded number of loop
iterations, widen increasing numeric ranges and container shapes to ensure
termination. Unknown calls invalidate mutable heap and dynamic global facts.
Immutable SSA temporaries retain value facts while live, but aliases do not
retain facts about mutable container contents.

Example: `x = 1; if condition: x = 's'` gives `int | str` at the join. An annotation
`x: int` does not remove `str`. A local exact-type guard refines only its success
edge. A source-level `isinstance(x, int)` test is not intrinsically trustworthy:
`isinstance`/`int` may be rebound, subclasses may behave differently, and arbitrary
metaclasses may customize instance checks. Compiler-inserted exact-type guards
use the runtime's actual builtin type, not a mutable global binding.

Container element inference is advisory unless mutation and aliases are proven
absent. Empty `list[T]` annotations do not enforce members. Calls to unknown
functions return `Unknown`; interprocedural summaries require proven callable
identity and default/closure dependencies, invalidated when those assumptions fail.
Class instances remain dynamic until layout, descriptors and mutation are proven.

## Integer specialization contract

For Python-compatible addition:

1. Preserve evaluated original operands and evaluation order.
2. Verify exact builtin `int` types; subclasses and booleans use generic dispatch.
3. Attempt lossless `int64_t` conversion. A conversion range miss routes to the
   original boxed operation; unrelated errors propagate.
4. Check addition bounds before evaluating signed addition. Portable checks must
   not themselves overflow. `__builtin_*_overflow` can be a probed toolchain path,
   with a portable checked alternative.
5. On overflow, call `PyNumber_Add` with original objects, retaining arbitrary
   precision. A failed guard is not an `OverflowError` visible to Python.
6. Box successful results when required and propagate allocation failures.
7. Prove that changed allocation and identity behavior is allowed at that boundary;
   otherwise keep the original boxed operation.

Subtraction/multiplication need separate checked implementations; division must
handle zero, floor-versus-truncation, and minimum-integer divided by `-1` without
undefined C behavior. Shifts validate negative counts and excessive widths.
Never substitute native division or remainder based only on integer type facts.
Initial optimization work should prefer proven nonescaping numeric regions over
broad replacement of arbitrary public function returns.

## Floating point and booleans

Exact float guards permit considering `double` specializations only after
verifying target representation and operation semantics. Preserve signed zero,
NaNs, infinities, overflow behavior, zero-division exceptions and boxing effects.
Disable fast-math, reassociation and contraction when they could alter Python
results. `pow`, division and mixed integer/float conversion require independent
semantic contracts; do not extrapolate multiplication's rules to them.

A truth conversion of an unknown object uses Python protocols and may raise.
Native booleans are valid only after a checked truth operation or a proof about
an exact builtin value. Boolean `and`/`or` return operands, not coerced booleans.

## Specialization and fallback

Specializations are private entry points with versioned guard and ownership
contracts. Public Python-compatible wrappers perform ordinary argument binding,
keywords, defaults and error handling. Never dispatch a cached specialization
solely from a prior invocation or annotations. Recheck callable/global/closure
assumptions when relevant; do not bypass monkeypatching or an overridden method.

Prefer entry guards and local slow paths. Initial releases have no general
mid-function deoptimization engine. Unsupported frames, unknown aliases or a
need to reconstruct arbitrary interpreter state cause conservative whole-function
fallback before execution. A failed guard cannot rerun preceding effects.
Limit the number of versions and native region size to avoid code explosion.

Diagnostics show declared type, inferred possibilities, physical representation,
evidence and reasons specialization was declined. False precision is a compiler
bug; imprecision is an acceptable reason for fallback.

## Required proof tests

Before enabling a specialization, test integer boundaries and huge values,
subclasses, overloaded operators, boolean operands, incorrect annotations,
mutated globals, identity escape, boxing failure, negative zero, NaNs and
exception order. The same observable contract applies at O0 through O3.
See [Testing](TESTING.md) and [Optimizations](OPTIMIZATIONS.md).
