# Calcium IR (CIR)

Status: proposed; see [Architecture](ARCHITECTURE.md), decision D03. CIR is a
compiler-owned contract, independent of the Python AST and C syntax.

## Representation

A `Program` contains target metadata, modules, constants and a source table.
A `Module` has a qualified Python name, ordered initialization function, imports
and function/class descriptors. A `Function` includes calling convention,
parameters/default metadata, scopes/cells, blocks, exception regions and source
span. Python-visible metadata remains distinct from native implementation names.

A `Block` has a stable ID, typed arguments, ordered instructions and exactly
one terminator. Immutable temporaries are assigned once. Rebinding lives in
explicit local/global/cell slots; block arguments merge temporary values, avoiding
an initial memory-SSA implementation. Values carry separate semantic type,
physical representation and evidence provenance; see [Type System](TYPE_SYSTEM.md).

Every instruction carries opcode, operands, optional result, span, effect set,
ownership signature and optimization provenance. Potentially raising operations
terminate a block with `invoke`, splitting normal result and error successors.
No use of a failed result is allowed. Serialize versioned CIR as deterministic
JSON for tools and readable text for humans. Neither format is a promised stable
public interchange ABI until a schema version is published.

## Operation families

| Family | Examples | Required information |
| --- | --- | --- |
| Bindings | `load_local`, `store_local`, `load_global`, `load_cell` | Slot identity, unbound check, reference behavior |
| Objects | `constant_object`, `get_attr`, `set_item`, `allocate_list` | Allocation/mutation/identity effects and errors |
| Python operations | `py_add`, `py_compare`, `py_truth`, `py_call` | Ordered operands, runtime dispatch, error edge |
| Imports | `import_name`, `import_from` | Level, fromlist, globals and runtime hook dispatch |
| Ownership | `retain`, `release`, `move` | Owned/borrowed provenance and cleanup obligations |
| Guards | `guard_exact_type`, `try_unbox_i64`, `checked_add_i64` | Success facts, guard-failure edge distinct from exception |
| Native values | `const_i64`, `add_f64`, `box_i64` | Proven preconditions, checked overflow, possible boxing errors |
| Function/class | `make_function`, `make_cell`, `build_class` | Defaults, decorators, closure state, runtime metaclass path |
| Control | `jump`, `branch`, `invoke`, `return`, `raise` | Block arguments and active exception/cleanup state |
| High-level regions | `with_region`, `finally_region`, `suspend_region` | Structured semantics before core lowering |

Effects include `reads_binding`, `writes_binding`, `reads_heap`, `writes_heap`,
`allocates`, `may_raise`, `may_call_python`, `observes_identity`, and
`releases_reference`. Unknown Python calls are conservative barriers. A
`release` can execute finalizers and mutate globals; it is not a pure bookkeeping
operation. Block order expresses effect order; the first IR does not require
memory tokens. Passes must query effect/dependency analysis before movement.

## Example: default Python-compatible addition

Input: `def add(a, b): return a + b`. The following is illustrative CIR, not an
implemented parser format:

```text
function add(a: object borrowed, b: object borrowed) -> object owned:
entry:
    invoke py_add(a, b) -> normal(result: object owned), error
normal(result):
    return move(result)
error:
    propagate_exception
```

A guarded specialization branches on exact `int` checks, then lossless conversion
to `i64`. Failed type/range guards jump to `py_add` with the original objects.
`checked_add_i64` branches on overflow before any overflowing C expression is
evaluated. Overflow also reaches `py_add`; successful native addition reaches a
raising `box_i64` and then returns the owned object. Errors from allocation or
actual runtime operations propagate; they are never mistaken for guard failure.
CIR keeps original operands alive until either path no longer needs them.

Initially restrict this optimization to cases whose identity/lifetime obligations
are proven and tested; a successful integer computation alone does not justify
eliminating every intermediate Python object. No specialization replays already
performed side effects.

## Lowering rules

- Expressions evaluate operands in Python's order. In `f(g(), h())`, load `f`,
  call `g`, then `h`, then call the captured callable; a later binding change
  does not retroactively change that loaded value. C argument evaluation order
  cannot implement this implicitly.
- `and`/`or` become branches preserving operand values; truth tests can raise
  or execute user code. Chained comparisons evaluate shared operands once.
- Assignments evaluate RHS before binding targets; unpacking and augmented
  assignment have dedicated order rules. Local reads carry an unbound sentinel
  separate from Python `None`.
- Loops have header/body/latch/else/exit blocks. Iterator exhaustion is distinct
  from other exceptions. `break` skips `else`; `continue` crosses cleanup only
  according to the enclosing regions.
- Returns/breaks/continues/raises crossing `finally` carry a pending-control record.
  Cleanup may replace the pending return or exception. Exception handlers track
  active handled exceptions and delete handler-name bindings at the required time.
- Closures use shared CPython cells; nested functions never capture a snapshot
  of a mutable binding. Default values are evaluated once at function creation.
- Decorator expressions and application order, class namespace preparation,
  descriptors and metaclasses use dedicated high-level operations with runtime
  lowering. They are fallback-only until their lowering is validated.
- Comprehensions model target-version scope isolation. Generators/async require
  resumable state, active exceptions and cleanup across suspension. Keep a whole
  function fallback until that model is implemented; do not flatten suspension
  into ordinary calls.
- Pattern matching preserves binding behavior, protocol calls and guards.
  Unsupported matching falls back at the enclosing valid boundary.

High CIR can describe a feature before native lowering is available. Such an
operation must lower to a declared fallback boundary or produce an unsupported
feature diagnostic; the backend may never silently skip it.

## Fallback boundaries

Fallback code objects preserve original source, filename and target compilation
flags. The compiler selects whole functions before execution, or an entire module
when mixed execution cannot preserve scope/definition behavior. No textual
`exec(fragment, globals, locals)` is permitted as a replacement for an arbitrary
CIR expression. A function fallback preserves the original Python function object;
substitution of a native wrapper occurs only when the support contract allows it.
A module delegated wholesale has no native coverage claim.

## Ownership and exception invariants

An owned object value has exactly one release or transfer on every reachable
path; retaining creates a separate ownership obligation. Borrowed values carry
an owner/lifetime witness. A call that can invalidate a borrowed owner requires
retention before crossing it. Merging edges have consistent ownership and active
exception state. Unboxed values are not Python objects and cannot be passed to
object operations without boxing.

Exception edges have the CPython error indicator set. Success edges must satisfy
the operation's success contract. Guards do not leave a stale exception behind.
Handlers move the raised exception into explicit state; re-raise and cleanup
preserve cause, context and active-handler semantics. See [Runtime](RUNTIME.md).

## Validator and pass protocol

Validate after building, lowering and every optimization pass. Reject duplicate
IDs, missing terminators, unknown targets, argument mismatches, non-dominating
uses, invalid representation conversions, missing raising edges, unproven native
preconditions, use after transfer, leaked ownership and borrowed-reference escape.
Validate exception-region nesting and every path crossing structured cleanup.

Each pass declares required analyses, changed effects, invalidated facts and a
semantic justification. On invalid output compilation fails with an internal
error and the last valid IR dump in debug mode; it does not emit unsafe C.
Persist no target memory addresses or absolute checkout roots in CIR identities.
