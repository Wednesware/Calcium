# Calcium

Calcium is a proposed Wednesware ahead-of-time Python-to-C transpiler. It aims
to accelerate code that can be specialized safely while retaining dynamic
Python behavior through embedded CPython.

**Status: Phase 0 — architecture proposal awaiting maintainer review.** There is
no implemented transpiler, installable Calcium distribution, supported CLI, or
published performance result. The examples in the design documents specify
future behavior. Architecture approval is required before production work,
as requested in [CALCIUM_INFO.md](CALCIUM_INFO.md).

Planned capabilities include readable C11 output, optional GCC/Clang builds,
Python-compatible object operations, guarded native arithmetic, inspectable
optimization decisions, scripts and packages, and CPython extension modules.
Standalone execution without CPython and a general-purpose shared-library ABI
are separate research goals. Higher optimization levels will retain the same
compatibility contract; annotations alone will never authorize native types.

Calcium is separate from Kefir, Wednesware's Nuitka-based application builder.
It will neither replace nor depend on Kefir.

## Review the design

| Document | Review focus |
| --- | --- |
| [Architecture](docs/ARCHITECTURE.md) | Pipeline, dependency investigation, decisions and risks |
| [CIR](docs/IR.md) | Control flow, effects, validation and lowering |
| [Type system](docs/TYPE_SYSTEM.md) | Evidence, guards and specialization |
| [Runtime](docs/RUNTIME.md) | Ownership, exceptions, interpreters and C backend |
| [Optimizations](docs/OPTIMIZATIONS.md) | Pass contracts and optimization levels |
| [CLI and API](docs/CLI.md) | Proposed commands, options and result schema |
| [Compatibility](docs/COMPATIBILITY.md) | Explicit support boundary and import model |
| [Testing](docs/TESTING.md) | Differential correctness and benchmark methodology |
| [Roadmap](docs/ROADMAP.md) | Dependencies, acceptance criteria and approval record |

Start with the decision register in Architecture. Approval must identify which
decisions are accepted and resolve the blockers in Roadmap; opening or merging
this proposal alone does not record approval unless the review explicitly says so.
No license has been selected yet; maintainers must approve an open-source license
before distribution.
