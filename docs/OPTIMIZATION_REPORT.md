# Static optimization and fallback report

Use the report before choosing the next optimization to implement:

```sh
ca build path/to/package -O3 --optimization-report
ca check path/to/package -O3 --optimization-report
ca check path/to/package -O3 --optimization-report --format json
```

`check` analyzes the project without running its code or invoking a C compiler.
`build` adds the same report to an ordinary build. Neither command launches the
application. The report counts source definitions, rather than measuring execution
time, startup time, hot functions or a runtime fallback percentage.

Text reports go to stderr, preserving the normal artifact/check output. JSON
results contain `optimization_report` (report schema 1); the API exposes the same
field as `result.optimization_report`. Emitted C manifests also include it. Existing
`--report-fallbacks` and `--report-optimizations` remain available for the raw lists.
The API computes coverage even when strict mode rejects fallback. Reports describe
selected lowering, not a guarantee that compilation/linking succeeded.

## Counts and decisions

Every named function, async function, method, nested function and lambda in the
analyzed AST counts once. Conditional/redefined functions are separate source
definitions; they are not predictions about which definitions will run. Classes
are counted separately. Qualified names, source lines/columns and module source
hashes identify each definition. Namespace packages appear in module modes but
contribute no source functions.

A whole-module fallback makes every function in that module fallback, including
functions that would otherwise be eligible. Likewise, methods inside a fallback
class and nested functions inside a fallback function are not counted as native.
Each function records its selected fallback boundary and IDs for all known
enclosing blocking boundaries. Each module stores a `boundaries` map with the
corresponding compiler reasons, avoiding repeated copies for every method/function.
Module records also retain the located fallback list.

Primary reason counts assign each fallback function to the first compiler blocker
of its selected boundary; these counts sum to the number of fallback functions.
Known blocker category counts include additional reported boundaries and may
overlap. For example, a method can be blocked by both a module future import and
its enclosing class. Removing the primary blocker does not prove the function
will become native. Opaque fallback class/async bodies are not independently
lowered to invent a complete inventory of hypothetical internal optimizations.

Categories distinguish closures with captured variables, non-capturing nested
functions, decorators, annotations, classes, generators, async, lambdas, future
flags, reflection/dynamic features and other unsupported syntax. Reports include
the underlying compiler messages, rather than only category labels.

`optimization_decisions` preserves the actual optimizer pass records, including
operation counts and scalar-kernel guards/replay conditions where available. A
recorded pass with zero operations is not a successful speedup. Declined
specializations are not comprehensively diagnosed yet.

Coverage includes discovered project modules only. External packages and unresolved
dynamic imports are excluded. A semantic syntax error encountered after discovery
can leave a partial report (`complete: false`); analyzed/discovered module counts
make that visible. Parsing/discovery failures may prevent a report altogether.

## Local Discribe example

Analysis-only snapshot on 2026-10-24, project source fingerprint
`4d2249a758bb8ca4e894c5522282f792df4fab2d51467dce5c4d47fe11e449db`:

```text
Modules analyzed:   6 / 6
Functions analyzed: 38
Native lowering:    2
CPython fallback:   36
Classes analyzed:   6

Primary fallback reasons:
  Future compiler flags: 13
  Classes:              10
  Annotations:           7
  Closures:              6
```

`discribe.core` uses whole-module fallback because of
`from __future__ import annotations`. The collaboration, desktop and shell modules
use definition fallback. Across all known boundaries, categories overlap:
classes affect 18 functions, future flags 13, annotations 12, closures 6, async 5,
and other unsupported syntax 1. These are static counts for this source snapshot,
not measurements of Discribe's runtime bottlenecks. The application was not run.

This suggests investigating annotation/future-flag compatibility, class bodies and
closures before assuming more integer kernels will improve this application.
Startup timing and native runtime profiling remain separate work: `--profile-startup`
is not implemented by this change. For GUI applications, a fast start-and-exit
measurement says nothing about editing responsiveness or sustained interaction.
