# Calcium — High-Performance Python-to-C Transpiler

You are tasked with designing **Calcium**, a new open-source Wednesware publication that transpiles Python source code into optimized C.

Calcium is intended to improve Python program performance while preserving Python's behavior as faithfully as possible.

**Important: Begin with architecture design, not implementation.** Produce a comprehensive technical specification, identify difficult engineering decisions, and propose a staged development plan. Do not implement the transpiler until the architecture has been reviewed and approved.

## 1. Project Overview

**Name:** Calcium  
**Organization:** Wednesware  
**Implementation language:** Python  
**Input language:** Python 3  
**Output language:** C  
**Primary goal:** Balance native performance and Python compatibility  
**Compiler model:** Ahead-of-time transpilation with optional native compilation

Calcium should:

- Transpile Python into readable, valid C.
- Optimize statically analyzable code into efficient native operations.
- Preserve dynamic Python semantics through a compatibility runtime.
- Support individual scripts and complete Python packages.
- Generate native executables, shared libraries, or C source files.
- Support automatic type inference and optional Python type annotations.
- Provide multiple optimization levels.
- Integrate existing Wednesware libraries where appropriate.
- Remain extensible, modular, and maintainable.

### Relationship to Kefir

Kefir is Wednesware's existing Python application builder based on Nuitka.

Calcium is a separate project with a different purpose.

- **Kefir:** Packaging and building Python applications.
- **Calcium:** Transpiling Python into optimized C.

Do not modify, replace, or depend on Kefir unless a clear future integration opportunity is identified.

---

## 2. Fundamental Architecture

Design Calcium as a multi-stage compiler.

```text
Python Source
     |
     v
Source Loader
     |
     v
Python AST Parser
     |
     v
Semantic Analysis
     |
     v
Type Inference
     |
     v
Calcium Intermediate Representation
     |
     v
Optimization Pipeline
     |
     v
C Code Generator
     |
     +----> Generated C Source
     |
     v
Optional Native Compiler
     |
     +----> Executable
     |
     +----> Shared Library
```

### 2.1 Source Loader

Support both individual Python files and full projects.

Responsibilities:

- Discover source files and local modules.
- Resolve package structures and relative imports.
- Build a module dependency graph.
- Detect circular dependencies without rejecting valid Python programs.
- Preserve source locations for diagnostics.
- Identify dependencies requiring CPython interoperability.
- Respect Python's import initialization and caching semantics.

Do not assume that every import is statically resolvable.

Dynamic imports must remain supported through runtime mechanisms where possible.

### 2.2 Python AST Parser

Use Python's built-in `ast` module initially.

The parser must:

- Parse valid Python syntax supported by the selected Python version.
- Preserve line and column information.
- Produce useful syntax diagnostics.
- Handle decorators, comprehensions, generators, async functions, pattern matching, and other modern Python constructs.
- Avoid executing source code during analysis.

Keep parsing independent from code generation.

### 2.3 Semantic Analysis

Create a dedicated semantic analysis stage responsible for:

- Scope resolution.
- Local, global, and nonlocal variables.
- Closures and free variables.
- Function and class definitions.
- Name binding.
- Control-flow analysis.
- Exception handling.
- Import semantics.
- Dynamic attribute access.
- Evaluation order.
- Python object lifetime requirements.

Pay particular attention to Python's dynamic name binding and runtime introspection.

Semantic analysis must never assume that an annotation enforces a runtime type.

### 2.4 Type Inference

Implement automatic type inference with optional annotations.

The inference system should recognize:

- Integers.
- Floating-point numbers.
- Booleans.
- Strings.
- Bytes.
- Lists, tuples, sets, and dictionaries.
- Functions and callable objects.
- Classes and instances.
- Union types.
- Optional values.
- Unknown or dynamically typed values.

Use flow-sensitive analysis where practical.

Example:

```python
def multiply(a: int, b: int) -> int:
    return a * b
```

Calcium should identify this as a candidate for native integer arithmetic.

However, Python annotations are not runtime guarantees. Native specialization must use proven types, guarded assumptions, or an explicitly selected compilation contract.

Do not silently change Python behavior.

Python integers are arbitrary precision. A native C integer cannot universally replace a Python integer without overflow checks or fallback logic.

### 2.5 Calcium Intermediate Representation

Design a dedicated **Calcium IR (CIR)**.

Do not directly translate Python AST nodes into C.

The IR should support:

- Typed and dynamically typed values.
- Basic blocks.
- Explicit control flow.
- Function calls.
- Object allocation.
- Reference ownership.
- Exception propagation.
- Closures.
- Runtime operations.
- Type guards.
- Native arithmetic.
- Source mapping.
- Optimization metadata.

Consider SSA-style representation or a hybrid IR, but evaluate complexity before deciding.

Document how high-level Python constructs are lowered into CIR.

### 2.6 Optimization Pipeline

Provide configurable optimization levels.

| Level | Purpose |
|---|---|
| `-O0` | Minimal optimization, debugging-oriented |
| `-O1` | Safe local optimizations |
| `-O2` | Aggressive general-purpose optimization |
| `-O3` | Advanced specialization and performance optimization |

Potential optimizations include:

- Constant folding.
- Constant propagation.
- Dead-code elimination.
- Common-subexpression elimination where semantics permit.
- Control-flow simplification.
- Function specialization.
- Type specialization.
- Native arithmetic.
- Loop optimizations.
- Escape analysis.
- Allocation elimination.
- Boxing and unboxing optimization.
- Redundant reference-count operation elimination.
- Function inlining.
- Branch simplification.

Optimization passes must explicitly document their assumptions and semantic safety requirements.

Do not reorder operations when doing so would change observable Python behavior, including exceptions, side effects, operator overloading, or object identity.

Higher optimization levels must not silently sacrifice Python compatibility.

---

## 3. Hybrid Runtime Architecture

Calcium should generate native C operations whenever semantics can be preserved, falling back to CPython when necessary.

### Native Fast Paths

For proven or guarded types, generate specialized operations.

Example:

```python
def add(a: int, b: int) -> int:
    return a + b
```

Possible generated C fast path:

```c
static inline long calcium_add_long(long a, long b) {
    return a + b;
}
```

This example is only valid under an explicit bounded-integer contract. It is not a generally correct implementation of Python integer addition.

For default Python-compatible compilation, account for arbitrary-precision integers, overflow, dynamic calls, and type assumptions.

### CPython Fallback

Use CPython APIs for operations that cannot safely be translated into native equivalents.

Examples:

- Dynamic attribute access.
- Reflection.
- Dynamic imports.
- Complex metaclasses.
- Arbitrary operator overloading.
- Unsupported extension-module behavior.
- Runtime type manipulation.

Use `PyObject*` where needed.

### Runtime Modes

Investigate two possible execution modes:

**Embedded CPython mode**

Generated programs initialize or attach to a CPython interpreter.

Advantages:

- Greater compatibility.
- Existing Python ecosystem support.
- Easier handling of dynamic features.

**Standalone runtime mode**

Generated programs use Calcium's own runtime without CPython.

Advantages:

- Potentially smaller dependency footprint.
- Greater optimization freedom.

Disadvantages:

- Much more difficult to achieve Python compatibility.
- Requires implementing substantial Python runtime behavior.

Recommend embedded CPython as the initial compatibility foundation.

Treat standalone runtime support as a separate long-term engineering goal, not an initial guarantee.

### Runtime Safety

Design explicit rules for:

- Reference counting and ownership.
- Borrowed versus owned references.
- Garbage collection.
- GIL requirements.
- Exception propagation.
- Object identity.
- Finalizers and weak references.
- Thread safety.
- Interpreter initialization and shutdown.

Never generate undefined C behavior as an optimization.

---

## 4. C Code Generation

The C backend should generate portable, maintainable code.

Requirements:

- Prefer C11 unless another standard is justified.
- Generate `.c` and `.h` files where appropriate.
- Separate generated program code from runtime support.
- Use deterministic symbol naming.
- Avoid symbol collisions.
- Preserve source mappings.
- Support multi-module compilation.
- Produce reproducible output where practical.
- Avoid unnecessary dependencies.
- Emit useful comments in debug mode.

Generated C should be reasonably readable for developers inspecting compiler output.

### Example

Input:

```python
def square(x: float) -> float:
    return x * x
```

An optimized specialization might produce:

```c
double calcium_square_f64(double x) {
    return x * x;
}
```

However, this specialization must only be used when runtime checks or proven assumptions make it valid. The public Python-compatible entry point must preserve Python's normal calling behavior.

---

## 5. Output Formats

Calcium must support three main output types.

### Generated C

Produce C source files without requiring a C compiler.

```bash
calcium transpile main.py
```

Example output:

```text
build/
├── main.c
├── main.h
└── calcium_runtime.h
```

All generated output should be compilable using documented dependencies.

### Native Executables

```bash
calcium build main.py
```

Calcium should invoke an available C compiler.

Initially support:

- GCC.
- Clang.

Other toolchains can be added later.

### Shared Libraries

```bash
calcium build main.py --target shared
```

Define the intended ABI clearly.

Distinguish between:

- CPython extension modules.
- General-purpose native libraries with an explicit C ABI.

These have different initialization, ownership, and interoperability requirements.

Do not treat them as interchangeable.

---

## 6. CLI Design

Create a clean command-line interface.

Proposed commands:

```bash
calcium transpile main.py
calcium build main.py
calcium run main.py
calcium check main.py
calcium inspect main.py
calcium benchmark main.py
calcium info
```

### Command Responsibilities

**`transpile`**

Generate C source without invoking a native compiler.

**`build`**

Transpile and compile a native artifact.

**`run`**

Build and execute a Python program.

**`check`**

Analyze source code and report diagnostics without producing output artifacts.

**`inspect`**

Display compiler analysis information, such as inferred types, CIR, optimization decisions, and fallback usage.

**`benchmark`**

Compare execution performance against CPython.

**`info`**

Display Calcium version, supported Python versions, compiler availability, and runtime configuration.

### Shared Options

```text
-O0 / -O1 / -O2 / -O3
--output PATH
--target executable|shared|extension|c
--compiler gcc|clang
--python PATH
--debug
--verbose
--keep-c
--emit-ir
--emit-ast
--report-optimizations
--report-fallbacks
--help
--version
```

Define consistent option behavior and error handling.

For example, `--compiler` should only matter when a native compilation step is requested.

---

## 7. Wednesware Integration

Calcium should reuse existing Wednesware publications when they provide suitable functionality.

### Magnesium

Use **Magnesium** for logging, terminal colors, and presentation where practical.

Potential uses:

- Compiler diagnostics.
- Progress messages.
- Warnings and errors.
- Colored CLI output.
- Debug information.
- Optimization reports.

### Iodine

Use **Iodine** for interactive user input where appropriate.

Potential uses:

- Interactive project initialization.
- Configuration prompts.
- Compiler selection.
- Confirmation dialogs.

Noninteractive commands must work without prompts, particularly in CI environments.

### Integration Requirements

Before implementation:

1. Inspect the actual Magnesium and Iodine APIs.
2. Use their existing public interfaces rather than inventing APIs.
3. Check version compatibility and installation requirements.
4. Avoid introducing circular Wednesware dependencies.
5. Keep compiler internals independent from CLI presentation libraries.
6. Provide a graceful noninteractive interface for automation.

Other Wednesware libraries may be used when clearly beneficial, but avoid dependencies added only for branding consistency.

---

## 8. Python API

Calcium should also expose a programmatic interface.

Proposed usage:

```python
from calcium import Calcium

compiler = Calcium(
    optimization=2,
    target="c",
)

result = compiler.transpile("main.py")
```

Additional proposed operations:

```python
compiler.build("main.py")
compiler.analyze("main.py")
compiler.inspect("main.py")
```

Provide structured result objects containing:

- Success status.
- Generated artifacts.
- Diagnostics.
- Optimization reports.
- Runtime dependencies.
- Compilation metadata.
- Timing information.

Design the public API to remain stable even if compiler internals change.

---

## 9. Project Structure

Propose a modular structure similar to:

```text
calcium/
├── calcium/
│   ├── __init__.py
│   ├── __main__.py
│   ├── api.py
│   ├── cli/
│   ├── frontend/
│   │   ├── loader.py
│   │   ├── parser.py
│   │   ├── semantics.py
│   │   └── types.py
│   ├── ir/
│   │   ├── nodes.py
│   │   ├── builder.py
│   │   └── validator.py
│   ├── optimizer/
│   │   ├── manager.py
│   │   └── passes/
│   ├── backend/
│   │   └── c/
│   │       ├── generator.py
│   │       ├── emitter.py
│   │       └── toolchain.py
│   ├── runtime/
│   │   ├── include/
│   │   └── src/
│   ├── diagnostics/
│   └── config.py
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── compatibility/
│   ├── performance/
│   └── fixtures/
├── benchmarks/
├── examples/
├── docs/
├── pyproject.toml
└── README.md
```

This is a proposed starting point, not a mandatory structure.

Refine it based on actual module responsibilities.

---

## 10. Testing Strategy

Correctness is more important than benchmark performance.

### Unit Tests

Test individual compiler stages:

- Parsing.
- Scope analysis.
- Type inference.
- IR construction.
- Optimization passes.
- C generation.
- Diagnostics.

### Compatibility Tests

Execute identical programs using CPython and Calcium.

Compare:

- Return values.
- Standard output.
- Standard error where meaningful.
- Exception types.
- Observable side effects.
- Exit status.
- Object behavior.

Include difficult cases:

- Integer overflow.
- Dynamic typing.
- Closures.
- Decorators.
- Generators.
- Context managers.
- Exceptions.
- Inheritance.
- Metaclasses.
- Circular imports.
- Async functions.
- Mutable default arguments.
- Operator overloading.
- Reflection.
- Extension modules.

Normalize nondeterministic values only when justified.

### Differential Testing

Build a test harness that:

1. Runs a test using CPython.
2. Transpiles it with Calcium.
3. Compiles the generated C.
4. Runs the resulting artifact.
5. Compares observable behavior.
6. Reports mismatches.

Use property-based testing and fuzzing where practical.

### Optimization Tests

Run compatibility tests at every optimization level.

No optimization pass may be accepted solely because it improves performance.

---

## 11. Performance Benchmarking

Benchmark Calcium against CPython and, optionally, Nuitka and other relevant Python compilers.

Use equivalent workloads and document compiler settings.

### Benchmark Categories

- Integer arithmetic.
- Floating-point arithmetic.
- Tight loops.
- Recursive functions.
- Function-call overhead.
- List processing.
- Dictionary operations.
- String processing.
- Object creation.
- Class method calls.
- Numerical algorithms.
- Import-heavy applications.
- Mixed native and dynamic operations.

### Metrics

Measure:

- Execution time.
- Peak memory usage.
- Compilation time.
- Generated C size.
- Executable size.
- Runtime initialization overhead.
- Percentage of operations using CPython fallback.

### Benchmark Methodology

- Include warm-up runs where appropriate.
- Use repeated measurements.
- Report medians and variability.
- Separate compilation time from execution time.
- Validate correctness before recording performance.
- Avoid including interpreter startup when measuring only steady-state computation.
- Also report end-to-end startup performance separately.
- Document hardware, Python version, compiler version, optimization flags, and environment.

Calculate speedup as:

```text
Speedup = CPython execution time / Calcium execution time
```

Do not promise fixed speedups.

Programs relying heavily on CPython fallback may see limited gains or even regressions.

---

## 12. Development Milestones

### Phase 0 — Architecture and Feasibility

**Design only. No production implementation.**

Deliver:

- Architecture document.
- Python compatibility model.
- CIR specification.
- Type system design.
- Runtime strategy.
- C backend design.
- Ownership and exception model.
- CLI specification.
- Public API specification.
- Optimization framework.
- Dependency analysis.
- Testing strategy.
- Performance methodology.
- Risk assessment.

Identify unresolved architectural questions and recommend solutions.

### Phase 1 — Minimal Compiler Prototype

After architecture approval:

- Parse basic Python programs.
- Lower simple constructs into CIR.
- Generate C for basic expressions.
- Support simple functions.
- Support primitive operations.
- Produce basic diagnostics.
- Compile and run small programs.

### Phase 2 — Python Runtime Integration

- CPython interoperability.
- Dynamic Python objects.
- Exceptions.
- Function calling.
- Imports.
- Reference management.
- Basic classes and containers.

### Phase 3 — Type Inference and Optimization

- Flow-sensitive type inference.
- Native specialization.
- Type guards.
- Arithmetic optimization.
- Control-flow optimization.
- Optimization levels.
- Optimization reporting.

### Phase 4 — Project and Package Support

- Multi-module compilation.
- Package resolution.
- Shared libraries.
- CPython extension modules.
- Native executable generation.
- Build caching.

### Phase 5 — Advanced Compatibility

- Generators.
- Async behavior.
- Complex class semantics.
- Dynamic imports.
- Reflection.
- Extension-module interoperability.
- Advanced exception handling.

### Phase 6 — Performance and Stability

- Differential testing.
- Fuzzing.
- Profiling.
- Runtime optimization.
- Benchmark comparisons.
- Cross-platform testing.
- Documentation and release preparation.

These milestones are conceptual. Refine their dependencies and order during architecture design.

---

## 13. Engineering Constraints

Follow these principles throughout development.

**Correctness before speed.** An optimization must not silently change Python semantics.

**No fake compatibility.** Do not claim full Python compatibility before it is demonstrated through testing.

**No unsafe assumptions.** Type annotations, inferred types, and runtime observations must not be treated as universal guarantees.

**Modularity.** Frontend, IR, optimizer, backend, runtime, and CLI should remain independently testable.

**Predictability.** Compilation results and optimization decisions should be explainable.

**Transparency.** Developers should be able to inspect generated C, inferred types, and CPython fallback usage.

**Extensibility.** The architecture should permit additional optimization passes and potentially other backends without requiring a complete rewrite.

**Realistic scope.** Full Python compatibility is a long-term goal, not a requirement for the first working release.

---

## 14. Initial Codex Task

Before writing production code:

1. Inspect the existing repository, if present.
2. Research the actual public APIs of Magnesium and Iodine using available local source or repository access.
3. Evaluate the proposed architecture for feasibility.
4. Identify any fundamental limitations of the hybrid CPython/native approach.
5. Compare possible CIR designs.
6. Define precise type inference and specialization rules.
7. Explain how generated C will preserve Python exceptions, reference ownership, and evaluation order.
8. Specify how Python projects and modules will be compiled.
9. Determine how native functions and CPython objects will interoperate.
10. Define the boundary between Calcium's transpiler and optional native build tooling.
11. Produce a detailed architecture specification.
12. Produce a phased implementation roadmap with acceptance criteria.
13. Identify decisions requiring maintainer approval.

Pay particular attention to the tradeoff between compatibility and speed. Propose a design that can achieve meaningful native performance without making unsupported assumptions about Python programs.

### Required Deliverables

Produce the following documents:

- `docs/ARCHITECTURE.md`
- `docs/IR.md`
- `docs/TYPE_SYSTEM.md`
- `docs/RUNTIME.md`
- `docs/OPTIMIZATIONS.md`
- `docs/CLI.md`
- `docs/COMPATIBILITY.md`
- `docs/TESTING.md`
- `docs/ROADMAP.md`

Also produce a concise `README.md` explaining Calcium's goals, intended features, and development status.

The intended result is a well-designed, performance-focused Python-to-C transpiler that can grow into a serious Wednesware compiler project.