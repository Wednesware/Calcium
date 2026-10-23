# Changelog

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
