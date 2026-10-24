# Computation and startup benchmarks

`ca app.py --benchmark` measures complete processes, including interpreter startup
and imports. A short CLI workload can therefore be slower even when a numerical
function runs faster. No claim about Nitrogen's current performance follows from
the measurements below; Nitrogen has not been rerun in this change.

Time a function body after imports:

```sh
ca examples/compute.py --benchmark --function sum_squares --arguments '[1000000]' -O3 --repeat 9 --warmups 2 --format json
ca examples/compute.py --benchmark --function fibonacci --arguments '[25]' -O3 --repeat 9 --warmups 2 --format json
ca examples/compute.py --benchmark --function containers --arguments '[20000]' -O3 --repeat 9 --warmups 2 --format json
```

The file loads once per Python/native child; setup/imports happen before timing.
Each call receives the JSON arguments. Each iteration gets a fresh private working
directory, optionally copied from `--working-directory FIXTURE`. Copying, result
serialization and file hashing happen outside the measured call. Module state is
retained between calls, including warmups. Function benchmarks currently require a
Python file and builtin scalar/container results, rather than arbitrary objects.

Before reporting, both routes must agree on every measured iteration's type-tagged
result (including float bits), exception class/message, stdout/stderr and regular
file contents under that directory. Import stdout/stderr must also agree. These
checks do not establish complete behavioral equivalence: tracebacks, symlink and
file permission changes, absolute paths, network activity and external effects
are outside their coverage. Benchmarks execute the program; they are not sandboxes.

Reports include raw samples, medians, IQR, extrema and their median ratio. RSS is
whole-child peak memory, including the interpreter, imports and validation data;
it is not the isolated function's allocation footprint. Runs are sequential and
not CPU-pinned. Re-run on the deployment machine and inspect sample spread.

## Recorded local run

2026-10-24, Linux x86-64/glibc 2.44, CPython 3.14.8, GCC 16.2.1,
Calcium O3, two warmups and nine samples per route. See the commands above and
[all raw samples and environment metadata](benchmarks/compute-linux-python314.json).

| Workload | Python median | Calcium median | Python / Calcium |
| --- | ---: | ---: | ---: |
| Sum of squares, 1,000,000 integers | 39.764 ms | 1.385 ms | 28.70× |
| Recursive Fibonacci, n=25 | 5.514 ms | 0.944 ms | 5.84× |
| List/dict container workload, n=20,000 | 2.081 ms | 1.656 ms | 1.26× |

The first two use checked unboxed integer kernels; recursion calls the guarded C
scalar function directly. Containers use ordinary boxed native lowering. These
are workload-specific observations from one machine, not release guarantees or a
claim that all compiled code accelerates. Compilation costs are excluded. Startup,
large-integer overflow paths and compatibility execution can still dominate.
