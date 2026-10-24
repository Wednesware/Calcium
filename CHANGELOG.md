# Changelog

## 26.5 — 2026-10-24

- Add `--optimization-report` with static native/fallback function coverage, precise reason categories and qualified locations, enclosing fallback boundaries, JSON/API/manifest output and explicit partial-analysis reporting.
- Distinguish closure, nested-function, decorator, annotation and generic-function fallback reasons without changing native capability.

## 26.4 — 2026-10-24

- Add checked unboxed integer functions/loops and guarded direct scalar self-recursion at O2/O3.
- Lower ordinary exception handlers, finally, synchronous context managers, comprehensions, dynamic dict keys and set expansion natively.
- Add function-body benchmarks with typed result, exception, output and working-directory file checks; publish raw workload-specific samples and peak RSS.
- Add relocatable Linux CPython bundles with selected environment dependencies, transitive non-system ELF libraries and package resources.
- Preserve remaining architecture/platform/introspection limits explicitly in the status ledger.

## 26.3

- Build scripts/packages directly with `ca PATH`; select actions with options while retaining subcommands.
- Expand native function signatures, calls, comparisons, unpacking, formatting, assignments, deletion, assertions and raises.
- Report definition-level Python compatibility execution without forcing supported neighboring code into fallback.
- Add checked exact-integer arithmetic at O2 and literal-branch simplification at O3, preserving generic CPython slow paths.
- Cache native objects by preprocessed C/configuration and relink each build; verify hashes and tolerate cache failures.
- Fix folded NaN values/sign/constant identity and native import-hook fromlist values.
- Resolve libpython in relocated interpreter prefixes; strengthen CIR validation and expand differential/sanitizer checks.
- Add the maintainer-selected MIT license and update implementation/remaining-work documentation.

## 26.2

- Recursively discover and transpile imported project Python files into separate
  C translation units, then link the complete project.
- Add native import/from-import CIR operations and a compiled-module finder/loader
  that preserves CPython import caching, initialization order, relative imports,
  valid cycles, active import hooks and rollback after initialization failure.
- Accept package/project directories and `--module` entries, with `--project-root`,
  `--include-path` and `--include-all` for source layouts and dynamic local modules.
- Require native lowering for projects by default; make compatibility mode explicit
  with `--allow-fallback` and report native/fallback status per file.
- Support package extensions and namespace packages spanning declared source roots.
- Emit a complete module/dependency manifest, per-module CIR/AST/source maps and
  dependency fingerprints; compile every generated C file with GCC or Clang.
- Test compiled projects after hiding their original source trees, including
  relative/circular/dynamic imports, import hooks and extension submodules.
- Align CLI/API/package metadata at version 26.2 and provide both `ca` and `calcium`
  console commands.

The supported native language subset and CPython runtime requirements remain in
place. Standard-library/binary dependencies and package resources are not converted
or bundled. See [package documentation](docs/PACKAGES.md) for precise behavior.
