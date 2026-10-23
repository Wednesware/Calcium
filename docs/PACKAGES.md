# Package and project transpilation in 26.2

Calcium discovers project imports without importing or executing their Python
modules. Every discovered Python source file goes through its own frontend,
CIR, optimizer and C backend. The output contains one C translation unit per
source file plus a compiled import registry and runtime support. Imported project
modules execute their C entry points, with no original Python source files required
when all recorded modules are native.

## Compile a package or project

For a regular package with `__main__.py`:

```sh
ca transpile path/to/package --output build/c --emit-ir
ca build path/to/package --output build/myapp
./build/myapp
```

For a project directory, Calcium selects `__main__.py` or `main.py`. A directory
input includes every Python module below that directory, including modules selected
by dynamic imports. Hidden directories, virtual environments, build/dist output,
`__pycache__`, node_modules and common test-environment directories are excluded.
The source scan does not execute module initializers or build hooks.

An explicit module entry behaves like Python's `-m`:

```sh
ca build --module my_package.cli --project-root ./src --output build/myapp
ca run --module my_package --project-root ./src -- one two
```

A script entry automatically follows its statically discoverable imports:

```sh
ca build project/main.py --output build/myapp
```

Use `--project-root` to set the import root for a src layout, and repeat
`--include-path PATH` for additional pure-Python source roots, such as a vendored
dependency. These are explicit source roots, not permission to execute package
installation hooks. A Python dependency outside the declared roots remains an
external runtime dependency.

For variable-name or indirect dynamic imports, use a directory input or
`--include-all` to compile all Python modules in the root:

```sh
ca build project/main.py --include-all --output build/myapp
```

Literal `importlib.import_module` and `__import__` calls are discovered where their
names are statically known, including relative import_module calls with a known
package. Recognized unresolved dynamic imports generate C112 diagnostics. Arbitrary
reflection cannot be resolved statically; modules outside the included source set
are not claimed converted.

## Strict project compilation

Package, directory, explicit module and multi-file project inputs require native
lowering by default. If any included file contains unsupported syntax, the command
returns C101 diagnostics naming that file and emits no successful artifact. This
avoids presenting embedded Python source as a fully transpiled project.

`--no-fallback` explicitly enforces that rule for a single file too. An isolated
single-file script keeps the earlier default compatibility behavior.

`--allow-fallback` explicitly opts into compatibility mode. Supported files still
become native C modules, but unsupported files embed Python source in their own
C translation units and execute code-object fallback. Such modules are labeled
`fallback`, and overall mode is `mixed` when appropriate. This mode removes the
need for original project source files but is **not** full native C conversion.

The supported Python syntax is listed in [Implementation](IMPLEMENTATION.md).
Imports no longer select fallback. Defaults, annotations, classes, generators,
exception handlers and other previously unsupported native syntax remain subject
to the same language capability checks. No optimization level weakens this policy.

## Inspect the result

```sh
ca inspect project/main.py --format json
ca transpile project/main.py --output build/c --emit-ir --emit-ast
```

`manifest.json` schema 2 records:

- Every compiled module's qualified name, source hash, generated C filename and
  exported execution symbol, with package/namespace flags and mode.
- The entry module, source roots, dependency graph and unresolved dynamic imports.
- External imports, runtime requirements and a fingerprint of all source modules.
- Per-module CIR/AST sidecar filenames when requested.

The generated `program.c` contains only the registry and entry/extension glue;
`module_<readable_name>_<digest>.c` contains each module's executable C. Static
implementation symbols are isolated between translation units; the digest makes
exported executor and artifact names deterministic and distinct. Namespace packages
have no Python initializer, so they have a registry/path record without a C file.

`program.cir.json` retains entry CIR fields and adds a `modules` map. Source maps
also include all source modules. Native builds compile and link every generated
C file, and `--keep-c` retains those files beside the native output. C generation
alone does not require a C compiler.

## Import behavior and limitations

The runtime installs a per-interpreter compiled-module finder before the ordinary
PathFinder, after existing custom finders. Its module table contains no mutable
Python objects. CPython importlib creates modules, inserts partially initialized
modules into `sys.modules`, executes the C entry, connects parent package
attributes, and removes failed module initializations. This preserves lazy import
order, repeated-import caching, valid circular imports, retry after failure,
relative import levels, aliases and fromlists. Import statements invoke the active
builtin `__import__` hook; they do not bypass it with direct executor calls.

Each imported module has its qualified `__name__`, package context, spec, original
source-origin path and package search paths. Module entry execution initializes
enclosing packages and executes as `__main__`. Namespace search paths can span
multiple declared roots. A script file remains script execution: use `--module`
for package-relative entry imports rather than directly running a package file.

Compiled module identities are fixed by the manifest. Existing `sys.modules`
entries and earlier custom finders retain precedence; later sys.path changes do
not redirect a compiled name to a different source tree. This is a frozen project
registry, not a fully dynamic filesystem finder. Native modules return no Python
source to inspection tools; runpy/get_code execution of an imported native module
is not implemented. Exact runpy warning behavior, Python frames, function object
introspection and traceback presentation remain outside native compatibility.

Standard-library modules, binary extensions and modules outside declared roots
continue through normal Python import machinery. Binary files are never labeled
native Python-source conversion, and a binary module takes precedence over a
same-name `.py` candidate. Matching CPython, its standard library and external
dependencies are still required. Package data files, distribution metadata and
shared libraries are not bundled; deploy them separately. Paths in module metadata
identify the original source installation even when those sources are absent.

## Build a package extension

A package without `__main__.py` can be transpiled as a library or built as one
CPython extension containing all of its compiled modules:

```sh
ca build path/to/my_package --target extension
```

The output name must match the top-level package name and the target interpreter's
extension suffix. Importing that extension sets package search metadata and makes
its compiled submodules available through the registry. This is a CPython ABI,
not a general C shared-library ABI. A package executable needs `__main__.py` or an
explicit `--module package.entry`.

## Python API

```python
from calcium import Calcium

compiler = Calcium(target="executable", project_root="src", include_all=True)
result = compiler.build(module="my_package", output="build/myapp")
assert result.success, result.diagnostics
assert all(m["mode"] in {"native", "namespace"}
           for m in result.metadata["modules"])
```

`fallback=None` selects the automatic strict-project policy; `False` always rejects
fallback; `True` explicitly enables compatibility mode. Each of `analyze`,
`inspect`, `transpile`, and `build` accepts either a source/directory argument or
`module=`, together with project configuration on the immutable compiler instance.
