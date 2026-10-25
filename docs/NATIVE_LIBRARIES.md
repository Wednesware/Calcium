# Bonemarrow 26.8 native library scope

Imports below resolve to native C modules. Import availability does **not** mean
every Python API or option exists. Unsupported attributes/options raise native
errors; there is no interpreter fallback. Linux is the validated platform.

| Module | Implemented scope |
| --- | --- |
| `pathlib` | Posix `Path`, `/`, name/stem/suffix/parent/parts, home/resolve/expanduser, mkdir, text/bytes reads and writes, exists/is_file/is_dir/stat, relative_to/as_posix, iterdir, rglob with a single-component pattern |
| `os`, `os.path` | getcwd/getenv, environ.get, replace/unlink/fsync/fdopen; exists/isfile/isdir/join/abspath/dirname/basename |
| `tempfile` | mkstemp, gettempdir |
| `shutil` | POSIX copytree and rmtree with supported options; no complete shutil emulation |
| `hashlib` | sha256, update, digest, hexdigest, copy through OpenSSL EVP |
| `sqlite3` | connect, execute with positional/named parameters, cursor fetching/iteration, rowcount, commit/rollback/close and transaction context management; native DB-API error hierarchy |
| `json` | loads/dumps, arbitrary integers, Unicode escapes/surrogate pairs, hooks, default callbacks, sorting/indent/separators/ensure_ascii, circular-reference detection |
| `dataclasses` | Plain @dataclass, normal/default-factory fields, init/repr/eq, inheritance, __post_init__, recursive asdict; frozen/slots/order/kw-only and many field options are unsupported |
| `functools` | partial and keyword override |
| `datetime` | datetime construction, now/fromtimestamp, isoformat and common attributes; naive and UTC values, timezone.utc; no timedelta or general timezone database |
| `uuid` | UUID parsing, uuid4 from OS entropy, string/hex/bytes/int/version access |
| `secrets` | token_hex, token_urlsafe using OS entropy |
| `time` | time, monotonic/perf_counter, sleep |

Strings/bytes support selected UTF-8/UTF-8-sig/ASCII encode/decode error policies,
join, replace, split/rsplit, strip variants, find/count, prefix/suffix tests and
Unicode lower/upper (Unicode 16). F-string formatting supports common width,
alignment and numeric precision specifications; the whole format mini-language
is not implemented.

Native exceptions expose arguments and cause/context, but do not recreate Python
traceback objects. Classes have single inheritance and bounded descriptors;
metaclasses, multiple inheritance, subclassing scalar/library types, custom
allocation/attribute hooks, slots,
weakrefs and finalizers are unsupported. General generator functions, async/await,
bitwise operations and unrestricted reflection remain outside the contract.

JSON unpaired surrogate values, UTF-16/32 byte input and custom encoder/decoder
classes are unsupported. Path strings require valid UTF-8; arbitrary filesystem
byte names are not covered. File text I/O is a limited implementation, not Python's
complete buffering/newline/codec stack. Exact error messages and type/function
reflection can differ. Do not infer support for an unlisted API from an import.

For SHA-256/SQLite builds install `pkg-config`, OpenSSL development files and
SQLite development files (Debian/Ubuntu: `libssl-dev libsqlite3-dev`). Executables
link the installed C libraries and need compatible shared libraries at deployment.
Pure CaRT programs retain their C/math-only dependency set.

`tests/fixtures/bonemarrow` contains source programs used for differential tests,
including native libraries, lazy generators, filesystem processing, bytes/sorting,
and JSON/SQLite errors. Each is run separately under CPython and a native executable
with an empty PATH and invalid Python runtime paths.

To reproduce the Discribe core probe against your own checkout, build
`examples/native/discribe_core_probe.py` with `--include-path /path/to/discribe`.
Run it in a fresh working directory; it creates a `workspace` directory and uses
a deterministic clock/UUID solely to compare outputs. The application source is
not modified. The [recorded probe](benchmarks/discribe-core-26.8.json) corresponds
to the core source hash shown there. The [full-package report](benchmarks/discribe-26.8-blockers.json)
is analysis only and still reports unsupported features.

The committed Unicode case tables are generated from CPython's Unicode 16.0.0
database by `tools/generate_unicode_case.py`; regeneration requires that database
version and formatting the result with `clang-format`. Runtime casing reads the
committed C tables and has no Python dependency.
