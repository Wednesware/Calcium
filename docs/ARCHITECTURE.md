# Architecture proposal

Status: proposed, not approved. This document and its linked specifications
fulfill Phase 0 of [the project brief](../CALCIUM_INFO.md). No compiler is
implemented in this change. Recommendations below are decisions to review,
not claims of delivered compatibility.

## Objectives and feasibility

Calcium should generate inspectable C from Python through a dedicated IR,
use native operations when their semantics are proven, and delegate dynamic
operations to CPython. The difficult part is preserving Python behavior while
removing dispatch and allocation costs. Translating every operation into a C
API call can add overhead and is a correctness baseline, not a speedup strategy.

Initial recommendation: Python 3.12 as the first language/runtime target,
Linux x86-64, a conventional GIL-enabled CPython build, C11 and GCC/Clang.
This is a deliberately bounded validation target, not a claim that newer Python
versions are unsupported forever. Each additional minor version needs frontend,
annotation, API, ABI and differential testing. The installed development
interpreter does not establish supported targets.

The compiler host and selected target interpreter must have the same Python
minor version initially. Cross-version and cross compilation fail explicitly.
Use public CPython APIs and the full version-specific ABI initially; the limited
API does not remove the need to validate language behavior or embedding details.

## Pipeline and stage contracts

```text
Source manifest → AST + source table → scopes + CFG → type facts
    → high CIR → lowered CIR → validated optimization pipeline
    → C sources + maps + build manifest → optional native toolchain
```

| Component | Inputs → outputs | Responsibility |
| --- | --- | --- |
| Loader | entry/project root → source manifest, dependency graph | Read bytes with Python encoding rules; never import analyzed modules |
| Parser | decoded sources → AST, spans, syntax diagnostics | Use target-version `ast`; preserve original text and future directives |
| Semantics | AST → symbols, scopes, control-flow facts | Binding, cells, globals, nonlocals, order and unsupported-feature classification |
| Inference | scopes/CFG → evidence-tagged type facts | Conservative flow analysis; annotations remain hints |
| CIR builder | analyzed AST → high CIR | Ordered operations, explicit effects, exception edges and ownership |
| Lowering | high CIR → core CIR | Runtime calls, blocks, cleanup and checked native operations |
| Optimizer | valid CIR → valid CIR + decision log | Only contract-preserving rewrites |
| C backend | core CIR → `.c`, `.h`, runtime support, maps | No AST interpretation or dependency discovery |
| Toolchain | artifact manifest + selected Python → native artifact | Compiler probing, flags, linking and build diagnostics |
| API/CLI | configuration → structured result/presentation | Orchestration; internals never print or prompt |

Read files once into an immutable compilation snapshot. Record logical relative
paths, content hashes, encoding, target version, optimization settings and tool
versions. Detect changed files before accepting a cached build. Sort graph
serialization and symbol allocation deterministically. Do not execute decorators,
annotations, package initializers, imports or configuration code during analysis.
`ast.parse` alone is insufficient to detect all scope errors; use target-version
symbol-table/compile validation without evaluating resulting code.

## Source and project model

A single file is a script entry point. A project has an explicit root and module
entry, proposed as `--project-root PATH --module package.main`; a directory alone
is ambiguous and produces a usage diagnostic. Discover `.py` and package
`__init__.py` files under that root without following symlinks outside it by
default. Namespace packages require declared search roots. Resources and binary
dependencies are runtime dependencies, not automatically bundled assets.

The dependency graph records definite local imports, possible imports, external
imports and unresolved dynamic edges. Cycles are legal; strongly connected
components assist planning only. They never determine execution order. Retain
conditional imports as conditional. Respect shadowing and import hooks at runtime.
Never call `find_spec` on arbitrary packages at analysis time because resolution
can execute parent packages. See [Compatibility](COMPATIBILITY.md).

## Module layout and extensibility

Proposed Python modules: `frontend/{loader,parser,semantics,types}`,
`ir/{nodes,builder,lowering,validator}`, `optimizer/{manager,passes}`,
`backend/c/{generator,emitter,toolchain}`, `runtime/{include,src}`,
`diagnostics`, `config`, `api`, and `cli`. CIR and result objects have no
CLI dependency. Toolchain consumes artifacts, not AST. Runtime helpers have
versioned headers and operation contracts; a second backend can consume core CIR.
Do not add plugin execution during analysis; compiler extensions are explicit,
trusted configuration in a later phase.

## CIR choice

| Candidate | Strength | Cost | Recommendation |
| --- | --- | --- | --- |
| Direct AST-to-C | Easy prototype | Hides effects, cleanup and optimization invariants | Reject |
| Stack bytecode | Simple interpreter mapping | Hard dataflow and ownership at joins | Not the optimization IR |
| Full SSA including memory | Strong analysis | Complex exception, alias and reference lifetime modeling | Defer |
| SSA temporaries + explicit mutable slots/cells | Clear def-use, incremental lowering | Conservative alias/effect barriers | Adopt |

High CIR retains structured cleanup and resumable constructs until suitable
lowering exists. Core CIR uses blocks with block arguments, explicit error edges,
and explicit object/native representations. Unsupported regions do not become
arbitrary string snippets evaluated with guessed locals. See [IR](IR.md).

## Dependency investigation (2026-10-23)

Inspected sibling checkouts, not guessed package names or placeholder README
examples. Source links below identify the inspected committed implementations.
Magnesium's working README was modified locally; API conclusions use its source.
No third-party library was imported or installed during inspection.

| Publication | Observed interface and installation | Decision |
| --- | --- | --- |
| Magnesium | `magnesium.logging.log(text)`, `.sublog(text)`, `.print()`; `error.from_exception(exc)`; `magnesium.color.Color.red/reset/rgb(r,g,b)` | Optional presentation adapter; no compiler dependency |
| Iodine | `iodine.run(widget, global_keymap=None, alt_screen=False, term=None)`; `TextInput`; `SelectMenu`; `Option(label, value)`; returns `None` on cancellation | Defer until an explicitly interactive command exists |
| Nitrogen | `require(pub, rel=None)` resolves Wednesware publications | Never resolve/download implicitly inside compilation |

Evidence: [Magnesium logging](https://github.com/Wednesware/Magnesium/blob/3c425d421b62670c5bc1cbf7f6fb74bcc860fe1a/magnesium/logging.py),
[Magnesium colors](https://github.com/Wednesware/Magnesium/blob/3c425d421b62670c5bc1cbf7f6fb74bcc860fe1a/magnesium/color.py),
[Iodine API](https://github.com/Wednesware/Iodine/blob/15606d75e636f6b7aa1ea25aca6412813f608191/iodine/__init__.py),
[Iodine widgets](https://github.com/Wednesware/Iodine/blob/15606d75e636f6b7aa1ea25aca6412813f608191/iodine/widgets/select.py),
and [Iodine installation notes](https://github.com/Wednesware/Iodine/blob/15606d75e636f6b7aa1ea25aca6412813f608191/README.md).

Both READMEs recommend `n2 get <publication>`; neither inspected checkout has a
`pyproject.toml` or `setup.py`. Do not invent pip requirements named `magnesium`
or `iodine`. Iodine documents Python 3.12+ and inconsistently lists Nitrogen
26.56+ and 26.57+; provisionally require at least 26.57 for experiments, then
validate the exact installed publication. Its module imports POSIX `termios` and
calls `nitrogen.require` at import time. Its `Style` implementation calls
`Color.get`, absent from inspected Magnesium. No compatible release pair has
been established; version pinning and an import/style/cancellation smoke test
are prerequisites to enabling the adapter.

Magnesium `log.print()` writes stdout and constructors append ANSI reset codes.
Do not route diagnostics blindly through it: use structured core diagnostics,
plain stderr by default and optional `Color` strings for TTY rendering. JSON
and redirected output have no ANSI. Adapter absence must not affect compilation.
The inspected dependencies do not import Calcium or Kefir; do not introduce
reverse dependencies. No need for Iodine in batch commands, or for any dependency
added solely to repeat Wednesware branding.

## Public API boundary

`Calcium` owns immutable configuration, not a global interpreter or terminal.
`analyze`, `inspect`, `transpile` and `build` return structured result objects;
only `build` invokes a toolchain. Explicit `run`/`benchmark` execute user code.
No implicit package installation, shell command construction or source execution
occurs during analysis. The [CLI/API specification](CLI.md) defines fields,
errors and artifact paths.

## Fundamental limits and risk register

| Risk | Consequence | Mitigation / release gate |
| --- | --- | --- |
| C API overhead and boxing | Compiled programs can be slower | Baseline measurements, guarded kernels; no fixed speedup promise |
| Frames, tracing, `locals`, code objects | Native functions cannot transparently emulate every Python function | Whole-function fallback before execution; publish exclusions |
| Finalizers and aliasing | Reordering releases changes behavior | Effect barriers, ownership verifier, adversarial lifetime tests |
| Import hooks and mutation | Static module graph can be wrong | Runtime import machinery; graph is planning metadata |
| Overflow and floating-point differences | UB or wrong results | Checked arithmetic, exact-type guards, strict FP flags |
| Interpreter ABI / extension packages | Deployment requires compatible CPython | Versioned manifest and artifact compatibility checks |
| Packaging versus compilation | Executables may miss stdlib/data/dependencies | Document runtime needs; packaging kept separate from Kefir |
| Frontend version changes | Syntax acceptance differs from semantics | Per-minor adapters and matrix before extending support |
| Large compiler scope | Premature compatibility claims | Narrow vertical slices and measurable exit criteria |
| License not selected | Cannot promise a licensed release | Maintainer license decision before distribution |

## Maintainer decision register

All entries are **proposed / pending approval**. Record reviewer, date and
rationale in [Roadmap](ROADMAP.md) when resolved.

| ID | Recommended decision | Alternative / tradeoff |
| --- | --- | --- |
| D01 | Embedded GIL-enabled CPython, version-specific public API | Standalone runtime is a separate project-scale effort |
| D02 | First target Python 3.12, Linux x86-64, GCC/Clang C11 | Broader initial matrix multiplies validation work |
| D03 | Hybrid CIR with SSA temporaries and mutable slots | Full memory SSA later if justified |
| D04 | Safe default semantics at every optimization level | Bounded numeric contract needs a separate explicit proposal |
| D05 | Whole-function/module CPython fallback, fully reported | Reject unsupported constructs when fallback disabled |
| D06 | Python extension ABI first; reject general shared ABI initially | General C ABI requires versioned handles and lifetime contract |
| D07 | Magnesium optional; defer Iodine pending compatibility fix | Mandatory dependencies would impede CI and portability |
| D08 | Introspection exclusions and conservative fallback boundary | Full native frame emulation is not an initial commitment |
| D09 | Choose open-source license before release | No license inferred from sibling projects |

Production implementation starts only after explicit review of D01–D08 and the
Phase 1 acceptance contract. D09 blocks distribution rather than design work.

## Primary technical references

The baseline uses versioned Python 3.12 documentation rather than the moving
latest docs: [AST](https://docs.python.org/3.12/library/ast.html),
[C API ownership and errors](https://docs.python.org/3.12/c-api/intro.html),
[interpreter configuration](https://docs.python.org/3.12/c-api/init_config.html),
[import machinery](https://docs.python.org/3.12/reference/import.html).
Specific contracts are linked in the companion documents. These references
inform the proposal; they do not establish tested Calcium support.
