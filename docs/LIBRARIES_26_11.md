# Native Nitrogen dependencies — 26.11

The ten 26.10 import diagnostics represented eight distinct module names.
Bonemarrow now implements native APIs for those names and their package parents.
This is a bounded compatibility contract, not a complete port of Python's standard
library. There is no interpreter fallback. Linux/glibc is the validated platform.

| Module | Implemented native APIs |
| --- | --- |
| `re` | compile, match/search/fullmatch, findall, lazy finditer, split, sub/subn, escape; compiled-pattern equivalents; match group/groups/groupdict, start/end/span, expand, string/re/pos/endpos; common I/M/S/X/A/U flags, text and bytes |
| `subprocess` | run, Popen, check_call/check_output; CompletedProcess attributes and check_returncode; Popen communicate/wait/poll/terminate/kill, pid/args/returncode and stdin/stdout/stderr pipes; capture_output, text/UTF-8, input, check, timeout, cwd, env, shell string commands, file descriptor redirection, PIPE/STDOUT/DEVNULL |
| `zipfile` | ZipFile in r/w/x modes, stored/deflated members; read, namelist/infolist/getinfo, write/writestr, extract/extractall, testzip, close/context management, is_zipfile; ZipInfo filename/file_size |
| `urllib.request` | urlopen and urlretrieve for HTTP, HTTPS and file URLs; GET/byte-body POST, HTTP redirects, download progress callbacks; buffered response read(size), getcode/geturl/getheader, status/code/url/headers, close/context management |
| `urllib.error` | URLError and HTTPError with native OSError inheritance; reason, URL, status and response-header attributes |
| `threading` | Real POSIX Thread callbacks, start/join/is_alive, daemon/name at construction, ident, get_ident/current_thread/active_count; Lock, RLock, Event and their acquire/release/wait/context operations |
| `traceback` | format_exc, format_exception, format_exception_only, print_exc/print_exception; captured generated-code filenames, function names and line numbers, exception causes/context; limit=None or zero |
| `importlib`, `importlib.util` | import_module for compiled registry modules, including relative names; find_spec, spec_from_file_location, module_from_spec, loader.exec_module, invalidate_caches |

Other APIs remain explicit attribute or operation errors. Additional changes
needed by Nitrogen include JSON load/dump streams, simple `@dataclass(slots=True)`
and `asyncio.get_running_loop()` with a native loop identity, is_running,
create_task and time.

## Dependencies and deployment

Regex uses PCRE2, ZIP uses libarchive, and HTTP/TLS uses libcurl. These established
C implementations avoid transpiling the Python implementations and their large
internal dependency graphs. HTTPS retains libcurl's certificate and hostname
verification. The new API tests include rejecting an untrusted certificate.

Install development files for libraries used by the program, alongside existing
OpenSSL/SQLite dependencies:

```sh
sudo apt-get install pkg-config libpcre2-dev libarchive-dev libcurl4-openssl-dev libssl-dev libsqlite3-dev
```

Builds probe pkg-config and report missing dependencies. Manifests record the
required packages/defines and thread flags. Programs using none of these modules
retain the base C/math dependency set. Compiled executables need compatible shared
C libraries and, for HTTPS, the system trust store; no Python runtime is embedded.
CaRT ABI is **6**, with a new cache namespace.

## Semantic boundaries

PCRE2 implements common Python regex patterns, including the actual Nitrogen
identifier/number patterns, groups and substitutions. It is not Python's regex
engine: engine-specific syntax, Unicode-version details and rare pattern behavior
can differ. LOCALE and unlisted flags are unsupported. Python-specific complete
Pattern/Match reflection and indexing protocols are not implemented.

Subprocess arguments must be strings/Paths or sequences thereof. Text mode is
UTF-8 with strict decoding. The implementation uses posix_spawn, nonblocking pipe
I/O and poll, draining stdout and stderr together. Timeout errors retain captured
bytes; run kills/reaps its child on timeout, while Popen timeouts permit retry.
Arbitrary preexec functions, pass_fds, close_fds=False, new sessions, Windows
options, full signal APIs and mixing buffered manual pipe reads with communicate
are outside scope. Popen is not a complete process-supervision framework.

ZIP extraction strips absolute/dot/parent member components and opens member
parents relative to a destination descriptor with O_NOFOLLOW. It rejects symlinks
and special-file members and refuses existing symlink traversal. Encrypted files,
append mode, file-like archive sources, full ZipInfo metadata, ZIP encodings and
per-member compression overrides are not implemented. ZIP metadata and timestamps
need not be byte-identical to archives written by Python.

urlopen buffers the response body; urlretrieve streams to a file. HTTP requests
are synchronous and retain the runtime lock during callbacks/header processing.
Connection pools, Request/custom openers, proxies/context configuration, cookie
jars, full HTTPMessage behavior and arbitrary protocols are not implemented.
HTTP headers are exposed as a native dictionary with a case-insensitive getheader
method on responses. This is not a replacement for a complete HTTP client API.

Thread callbacks share native objects under a runtime lock. At safepoints the
runtime can hand execution to another thread. Blocking joins, locks, events,
process communication and time.sleep release that lock. Suspended roots,
exceptions, environments, call depth and event loops are preserved independently
for GC and callback execution. This does not provide parallel execution of native
Python CPU loops. Thread subclassing, threading.local, Timer/Condition/Semaphore,
custom exception hooks and complete thread reflection are not implemented.
Finished threads release callback/argument references, and unreachable finished
thread wrappers can be collected. Non-daemon threads are joined at shutdown. With a still-running daemon, the
runtime retains its heap until the immediately following process exit, so it
does not destroy resources beneath a suspended daemon.

Traceback output uses native source locations and exception summaries, without
source excerpts or CPython traceback/frame objects. Full formatting options,
exception groups and exact output layout remain unsupported.

Compiled imports resolve only modules included at build time. Literal dynamic
imports participate in discovery; --include-all includes other package files.
Native modules with optional C libraries are available only when those libraries
were selected at build time; an explicit import selects them. find_spec returns
None for an unlinked module, and dynamic import fails explicitly. File-location
specs require a matching compiled origin. The source files may be
removed after building. Fresh isolated re-execution of an initialized compiled
module is rejected; normal import_module calls use the registry cache. Changing
sys.modules cannot install executable code into that registry. Downloading a new
Python package does not compile it: rebuild with that package included. There is
no runtime Python source loader, evaluator or JIT compiler in this release.

Slotted dataclasses enforce field names and hide __dict__, using native internal
field storage. They return a replacement class and expose __slots__. Slotted
inheritance, weakref slots and complete member-descriptor reflection are not
implemented. No memory-layout or dataclass performance improvement is claimed.

## Evidence

The regression suite exercises O0/O3 differential execution, UTF-8 and bytes
regex, lazy/empty matches, process pipes/status/errors/timeouts, ZIP round trips,
symlink traversal refusal, local HTTP redirects/POST/errors, untrusted TLS,
compiled imports after source removal, tracebacks, slotted classes, concurrent
allocation/GC and separate thread event loops. GCC and Clang ASan/UBSan exercise
the native libraries together.

Discribe with the supplied vendored Nitrogen graph now builds and --help runs.
Nitrogen import, publication-name helpers and InstallResult match Python. This
establishes those probes, not complete GUI/collaboration or package-installation
workflows. Runtime downloading/execution of new Python remains unsupported.
