# Bonemarrow built-ins — 26.10

A built-in name being available means CaRT implements the documented native
operations. It does **not** establish every CPython overload, protocol, reflection
feature, error attribute, or implementation detail. Unsupported behavior never
routes into an interpreter.

`ca --info --format json` includes `builtin_coverage`: an entry for every public
built-in of the compiler's Python, plus `__debug__` and `__import__`. Native
analysis results and emitted manifests include the same inventory. On Python
3.14, 129 names have native availability and 23 remain unsupported; counts can
vary with the compiler's Python version.

## Added in 26.10

| Names | Native scope |
| --- | --- |
| `any`, `all` | Lazy truth testing with short circuiting and exception propagation |
| `map`, `filter` | Lazy callbacks and iterable consumption; multiple map iterables; map strict length checking on Python 3.14 |
| `reversed` | Lists, tuples, strings, bytes, dictionaries, ranges and native sequence protocols |
| `iter(callable, sentinel)` | Repeated calls and sentinel comparison without eager consumption |
| `bin`, `hex`, `oct` | Arbitrary-precision integers and `__index__` |
| `chr`, `ord` | Unicode scalar values and one-character strings/one-byte bytes; lone surrogates remain unsupported |
| `pow` | Existing native power operations, arbitrary-precision modular exponentiation and modular inverses for negative exponents |
| `divmod` | Existing native integer/float floor division and remainder |
| `round` | Arbitrary integers and binary floats, ties to even, decimal places, integer return when digits are omitted, custom `__round__`; exact rational rounding avoids an intermediate decimal scaling error |
| `open` | Paths or descriptors, r/w/a/x and update modes, text/binary access, UTF-8/ASCII, strict/ignore/replace, descriptor ownership and native opener callbacks |
| `input` | UTF-8 stdin, prompts, line reads and EOFError; no readline editing/history integration |
| `exit`, `quit` | SystemExit behavior, like the native sys.exit implementation; no interactive site-helper presentation |
| `format` | The existing bounded native format engine and custom `__format__`, not the complete format mini-language |
| `delattr` | Supported instance/class namespaces; complete descriptor deletion remains outside scope |
| `vars(obj)` | The live native instance dictionary; no-argument frame reflection, module mappings and class mapping proxies remain unsupported |
| `id` | Native object identity, including copied tagged numeric values; CPython's immutable interning/allocation choices are not reproduced |
| `hash` | Numeric equality invariants, tuples/ranges/slices, selected native values and instance `__hash__`; randomized SipHash for text/bytes. Exact nonnumeric hash values and PYTHONHASHSEED emulation are not promised |
| `slice` | Construction, start/stop/step attributes and supported sequence slicing; full slice reflection/comparison protocols remain incomplete |

Files support context management, read/write, readline/readlines/writelines,
iteration, seek/tell, fileno/flush/close and selected metadata. Text I/O is a native
UTF-8/ASCII implementation, not Python's complete buffering/codec stack. Newline
translation supports the default universal-newline mode and bounded raw modes;
arbitrary newline specifications, seek cookies and all platform-specific I/O
semantics are not implemented. Read-ahead offsets survive closing shared file
descriptions, including closefd=False.

The 36 additional exception/warning types include SyntaxError, ArithmeticError,
ConnectionError and their subclasses, EOFError, GeneratorExit, StopAsyncIteration,
Warning and warning categories. Catching and subclass relationships work natively.
IOError/EnvironmentError alias OSError. Specialized constructor attributes,
tracebacks, warning emission machinery and exception groups are not complete.

Dictionary/set storage remains linear; this release adds the hash built-in, not a
hash-table implementation or complete custom-key semantics. General native object
and descriptor protocols remain bounded. A native type name does not imply every
method available on that CPython type.

## Still unsupported

- Dynamic execution/loading: compile, eval, exec and __import__.
- Full frame/class reflection: globals, locals, dir and super.
- New value/buffer runtimes: complex, bytearray, memoryview and frozenset.
- Async iteration helpers: aiter and anext.
- Exception groups: BaseExceptionGroup and ExceptionGroup.
- Ellipsis and NotImplemented singleton/protocol behavior.
- Interactive helpers: breakpoint, help, copyright, credits and license.

Those are runtime projects rather than aliases that can safely be enabled in a
name list. The inventory reports them explicitly. Use the separately selected
`--cpython` backend when their Python behavior is required.

## Syntax and dependencies

26.10 also adds starred assignment and literal/singleton/OR/wildcard match cases
with guards. Capturing, sequence, mapping and class patterns remain unsupported.
Container literals no longer have the former 128-element/64-entry frontend caps;
large native call scratch arrays use checked heap storage. Function parameter and
call argument limits remain 128.

Built-ins are separate from standard-library modules. The supplied vendored
Nitrogen tree has no reported built-in or listed syntax diagnostics. 26.11 adds
[selected native implementations](LIBRARIES_26_11.md) for its remaining module
imports. Downloaded Python packages still need rebuilding into the executable;
compiled importlib APIs do not implement a Python evaluator.
