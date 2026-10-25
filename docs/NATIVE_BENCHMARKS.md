# Bonemarrow package benchmarks — 26.7

These are bounded synthetic **whole-process** measurements, not a claim that
Discribe or arbitrary applications run natively. All fixtures compare stdout,
stderr, exit status and private-directory file hashes before accepting samples.

Reference: AMD Ryzen 5 7600X, Linux x86_64, system CPython 3.14.8, GCC 16.2.1,
O3, 2 warmups and 9 measured samples per runtime, shuffled with seed 267.
The runtime labels distinguish ordinary CPython, Calcium's explicit CPython
backend, and Bonemarrow. Compilation is outside the timed executable runs.

| Workload | Python ms | CPython backend ms | Bonemarrow ms |
| --- | ---: | ---: | ---: |
| Single-file pass | 9.879 | 10.399 | 0.583 |
| Package startup | 10.602 | 9.324 | 0.576 |
| One-module import | 10.017 | 9.147 | 0.582 |
| Multi-module import | 10.386 | 9.551 | 0.601 |
| 100,000 cross-module calls | 19.534 | 31.148 | 7.793 |
| 10,000 repeated imports | 11.540 | 12.105 | 1.007 |
| Circular imports | 10.452 | 9.632 | 0.588 |
| Package initialization | 10.075 | 8.993 | 0.584 |
| 50-module dependency chain | 13.386 | 9.753 | 0.555 |

The native pass executable is 94504 bytes. Median measured peak child RSS is 2.02 MiB for Bonemarrow and 11.51 MiB for Python. These values include the small C measurement-parent inheritance floor. Shared libraries and Python installation size are excluded from executable image sizes.

Native builds in this run took 1.068–2.518 seconds, with caching disabled for both
compiler backends. Cached native builds and relinking are tested separately;
they are not substituted for cold compilation measurements.

[Raw samples, CPU/RSS, IQR, compilation time, source hashes, compiler/environment,
image sizes and dependencies](benchmarks/packages-26.7-linux-python314.json)
are committed. Reproduce in an installed compiler environment:

```sh
python examples/native/package_benchmark.py --output /tmp/packages.json
```

The helper in `examples/native/measure.c` measures fork/exec/wait4 using a minimal
C parent, avoiding inheritance of a large Python measurement-parent RSS watermark.
RSS zero-accounting samples are null. Timings include loading, initialization,
computation, output and shutdown. They exclude measurement-helper startup, fixture
setup, compilation and validation. Program sources are hidden during each compiled
run, and each process gets an empty private working directory. Python uses `-B`
to avoid bytecode cache side effects. Package Python baselines use an explicit
`sys.path`/`runpy.run_module` bootstrap from that private working directory, which
adds interpreter-side bootstrap work; they are not pure import-only timings.
The compiler checkout was on PYTHONPATH during the reference run. The exact
baseline interpreter command is fixed in the benchmark source.

No CPU affinity or background-load control was imposed. Distributions/IQR matter;
these environment-specific medians are not universal startup/memory guarantees.
The source fixtures deliberately avoid unsupported libraries/classes/GUI code.
The 50-node workload is a dependency chain, not a test of every graph shape.
Cross-module calls use native tagged dispatch, not scalar intermodule inlining.
In this fixture Calcium's CPython backend is slower than ordinary Python; that
regression in generic compatibility-call overhead remains visible in the table.
Dictionary/set lookups are still linear and need their own scaling benchmarks.
GUI editing responsiveness, filesystem-heavy application performance, and sustained
large-application throughput were not measured.

The [26.6 single-file measurements](benchmarks/native-26.6-linux-python314.json)
remain historical results, from another run and backend version. Avoid treating
their medians as a controlled before/after experiment for 26.7.
