# 26.6 standalone benchmark measurements

Run the fixed three-runtime Linux suite:

```sh
PYTHONPATH=. python examples/native/benchmark.py --repeat 9 --warmups 2 --output native-results.json
```

Select the Python environment deliberately: the same interpreter runs the Python
baseline and backs the CPython executable. Published results used `/usr/bin/python`
3.14.8, rather than an editable-development virtual environment with extra startup
hooks. The CaRT executable does not use that interpreter. No GUI application is
launched. `--application PATH` optionally adds an analysis-only compatibility probe.

Each supported fixture is compiled at O3, including consumed/printed results.
Compile wall time is separate. Run order is shuffled deterministically per round
(seed 266). Before accepting samples, every repetition must agree on stdout,
stderr, exit status and private-working-directory file hashes. Fixture setup and
validation are outside measured execution. Unsupported native categories retain
located diagnostics and are not given native timing/speedup entries.

A small C helper measures high-resolution wall time, child CPU time and child peak
RSS using clock_gettime/wait4. This avoids inheriting the large Python measurement
process's RSS high-water mark. Helper startup is excluded; fork, output redirection,
executable loading, program execution and shutdown are included. The small C
helper's inheritance floor still affects absolute RSS; zero-accounting samples
are null, not claims of zero memory. Linux KiB are converted to bytes. Wall-time
samples include application stdout and process startup; they are not kernel-only
or GUI interaction measurements. CPU affinity/background load are not controlled.

Executable sizes cover only the main image: CPython binaries also need libpython
and the standard library, while CaRT still needs compatible platform C/math
libraries. These are not total deployment-footprint measurements.

## Recorded reference run

2026-10-25, AMD Ryzen 5 7600X, Linux x86-64, CPython 3.14.8, GCC 16.2.1,
O3, strict floating-point flags, two warmups and nine samples per runtime.
[Raw samples, CPU/RSS, IQR, source hashes, sizes and compile times](benchmarks/native-26.6-linux-python314.json).

| Whole-process workload | Python median | CPython backend | Native backend |
| --- | ---: | ---: | ---: |
| Empty pass | 8.295 ms | 8.803 ms | 0.501 ms |
| Sum of squares, n=1,000,000 | 49.314 ms | 10.604 ms | 2.165 ms |
| Recursive Fibonacci, n=25 | 14.377 ms | 10.418 ms | 1.354 ms |
| String concat/repeat fixture | 10.637 ms | 10.877 ms | 2.693 ms |
| One million compiled calls | 58.947 ms | 118.980 ms | 25.389 ms |

| Native workload | Median child peak RSS | Main executable bytes |
| --- | ---: | ---: |
| Empty pass | 2.016 MiB | 39,128 |
| Sum of squares, n=1,000,000 | 2.273 MiB | 43,432 |
| Recursive Fibonacci, n=25 | 2.281 MiB | 43,504 |
| String concat/repeat fixture | 2.637 MiB | 43,288 |
| One million compiled calls | 2.293 MiB | 43,584 |

The pass startup target below 1 ms was met on this reference machine. It is not
a portable guarantee. The arithmetic and recursion fixtures use checked scalar
kernels. The call fixture uses guarded direct C calls and tagged primitive values.
The CPython backend's call fixture is slower than Python; translating generic
boxed calls to C does not guarantee acceleration. Faster whole-process string
results do not establish faster sustained string processing after startup.

Containers, classes/objects, imports and filesystem fixtures are timed for Python
and the CPython backend, but **unsupported in native 26.6**. Real projects needing
these features, including Discribe, still require the CPython backend. No GUI
editing, broad application speedup or complete Python compatibility is claimed.
These fixed workloads are an acceptance baseline, not a substitute for future
allocation, object-model, import and real-application profiling campaigns.
