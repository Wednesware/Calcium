# Relocatable Linux CPython bundles

A bundle contains the compiled C executable, a matching CPython interpreter and
standard library, transitive non-system ELF dependencies, and application data.
It runs without a separately installed Python. It still requires a compatible
Linux C runtime/ELF loader, architecture and POSIX shell; it is not a CPython-free
runtime or a cross-compilation target.

```sh
ca my_package/ --bundle -O3 -o dist/myapp
./dist/myapp/run --help
ca main.py --bundle --resource data/ -o dist/script
ca main.py --bundle --no-dependencies -o dist/stdlib-only
```

`--bundle --run` builds the folder and starts its `run` launcher. The output must
be a new directory, so existing folders are preserved. Move/copy the complete
folder; invoke its launcher directly, rather than the underlying `program` binary.
The bundle is assembled in a temporary sibling directory before publication.
`manifest.json` lists hashes of shipped files and records runtime requirements.

By default the selected `--python` environment's site-packages are copied, including
package data and metadata. Use a dedicated environment containing the intended
runtime dependencies; the compiler does not install them. `--no-dependencies`
excludes site-packages. This is environment copying rather than dependency pruning
or wheel resolution; native wheels must already work on the target OS/architecture.
Absolute path entries in `.pth` files are omitted to avoid depending silently on
the build host. Editable installs, external-path plugin systems, executable `.pth`
hooks with host-specific assumptions and runtime downloads may need a conventional
installed dependency environment before bundling. Python development headers and
a C compiler are needed only on the build machine.

Compiled package trees automatically include non-code, non-hidden data. Extra
script data is explicit through repeatable `--resource PATH` (inside a declared
project source root). Python source/bytecode, hidden paths, VCS directories,
virtual environments, build/dist directories and resource symlinks are excluded
or rejected. Hidden data is currently not bundled. Bundle output must be outside
resource directories. Original project `.py` files are not shipped: supported code
is in C; allowed fallback definitions are already embedded and remain reported.
Dynamic local imports still need `--include-all` as described in [Packages](PACKAGES.md).

The compiled importer relocates `__file__`, package search paths and entry metadata.
`importlib.resources.files(package)` and resources located relative to `__file__`
work in the copied package tree. Native frame/source introspection limitations
still apply. The launcher locates the bundle relative to itself, sets the runtime
home and shared-library search directory, and isolates Python from user-site and
Python path/home environment overrides.

```python
from calcium import Calcium

result = Calcium(optimization=3, fallback=True).bundle(
    'my_package/', output='dist/myapp', resources=(), dependencies=True
)
if result.success:
    print(result.metadata['bundle']['directory'])  # run DIRECTORY/run
```

Tests move a bundle to a path containing spaces, delete its original source tree,
and run with invalid host PYTHONHOME/PYTHONPATH values. A second test deletes the
selected virtual environment and checks that its copied external dependency/data
still load. Standard-library zlib/sqlite extensions exercise copied ELF libraries.
