"""Real project compilation: imported source trees are hidden before execution."""

import json
import shutil
import subprocess
import sys
import sysconfig

import pytest

from calcium import Calcium
from calcium.frontend.project import discover

COMPILERS = [name for name in ("gcc", "clang") if shutil.which(name)]


def write_tree(root, files):
    root.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)


def run(command, **kwargs):
    command = list(map(str, command))
    if "calcium" in command:
        index = command.index("calcium") + 1
        if command[index] in {"build", "run", "check", "inspect", "benchmark", "transpile"}:
            index += 1
        command[index:index] = ["--cpython"]
    return subprocess.run(list(map(str, command)), capture_output=True, timeout=15, **kwargs)


def behavior(result):
    return result.returncode, result.stdout, result.stderr


def assert_no_sources_needed(root, executable, baseline):
    hidden = root.with_name(root.name + "-hidden")
    root.rename(hidden)
    try:
        actual = run([executable], cwd=root.parent)
        assert behavior(actual) == behavior(baseline), actual.stderr
    finally:
        hidden.rename(root)


PROJECTS = {
    "relative": {
        "main.py": """import app.arithmetic as a
from app.arithmetic import square as sq
from app import VALUE
import math
def local(n):
    import app.arithmetic
    from app import VALUE as value
    return app.arithmetic.square(n) + value
print(VALUE, a.square(4), sq(5), local(6), math.sqrt(9))
""",
        "app/__init__.py": "print('package init')\nVALUE = 7\n",
        "app/arithmetic.py": 'from . import VALUE\nprint("arithmetic init")\ndef square(n):\n    return n * n + VALUE\n',
    },
    "cycles": {
        "main.py": "import app.a\nimport app.b\nimport app.a as again\nprint(app.a.VALUE, app.b.VALUE, again is app.a)\n",
        "app/__init__.py": "print('package')\n",
        "app/a.py": "print('a')\nVALUE = 3\nfrom . import b\nprint(b.VALUE)\n",
        "app/b.py": "print('b')\nfrom . import a\nVALUE = a.VALUE + 1\n",
    },
    "duplicate_names": {
        "main.py": "from a.util import value as one\nfrom b.util import value as two\nprint(one(), two())\n",
        "a/__init__.py": "",
        "b/__init__.py": "",
        "a/util.py": "def value():\n    return 10\n",
        "b/util.py": "def value():\n    return 20\n",
    },
    "attribute_shadows_module": {
        "main.py": "from pkg import child\nprint(child)\n",
        "pkg/__init__.py": "child = 7\n",
        "pkg/child.py": "print('SHOULD NOT RUN')\n",
    },
    "conditional": {
        "main.py": "if False:\n    import unused\nimport helper\nprint(helper.value)\n",
        "unused.py": "print('SHOULD NOT RUN')\n",
        "helper.py": "value = 12\n",
    },
    "literal_dynamic": {
        "main.py": "import importlib\na = importlib.import_module('pkg.child')\nb = __import__('pkg.child')\nprint(a.VALUE, b.child is a)\n",
        "pkg/__init__.py": "",
        "pkg/child.py": "VALUE = 27\n",
    },
}


@pytest.mark.parametrize("compiler", COMPILERS)
@pytest.mark.parametrize("optimization", [0, 1])
@pytest.mark.parametrize("name", PROJECTS)
def test_project_native_without_source(tmp_path, compiler, optimization, name):
    root = tmp_path / "project"
    write_tree(root, PROJECTS[name])
    baseline = run([sys.executable, root / "main.py"])
    assert baseline.returncode == 0, baseline.stderr
    output = tmp_path / "program"
    result = Calcium(
        backend="cpython",
        target="executable",
        compiler=compiler,
        optimization=optimization,
        keep_c=True,
    ).build(root / "main.py", output=output)
    assert result.success, result.diagnostics
    assert result.metadata["mode"] == "native"
    assert not result.fallbacks
    manifest = json.loads((tmp_path / "program.calcium" / "manifest.json").read_text())
    records = manifest["modules"]
    assert len([r for r in records if r["c_file"]]) == len(PROJECTS[name])
    assert manifest["external_imports"] == sorted(set(manifest["external_imports"]))
    for record in records:
        assert record["mode"] == "native"
        text = (tmp_path / "program.calcium" / record["c_file"]).read_text()
        assert "ca_execute_source(" not in text
        assert record["executor"] in text
    assert_no_sources_needed(root, output, baseline)


@pytest.mark.parametrize("as_module", [False, True])
def test_package_main_and_metadata(tmp_path, as_module):
    root = tmp_path / "source"
    write_tree(
        root,
        {
            "pkg/__init__.py": 'print("init", __name__, __package__)\n',
            "pkg/__main__.py": "from .nested.mod import value\nprint(__name__, __package__, __spec__.name, value)\n",
            "pkg/nested/__init__.py": "",
            "pkg/nested/mod.py": "from .. import shared\nvalue = shared.VALUE\n",
            "pkg/shared.py": "VALUE = 42\n",
        },
    )
    baseline = run([sys.executable, "-m", "pkg"], cwd=root)
    compiler = Calcium(backend="cpython", target="executable", project_root=root)
    result = (
        compiler.build(module="pkg", output=tmp_path / "program")
        if as_module
        else compiler.build(root / "pkg", output=tmp_path / "program")
    )
    assert result.success, result.diagnostics
    assert_no_sources_needed(root, result.executable, baseline)


def test_project_root_directory_dynamic_modules(tmp_path):
    root = tmp_path / "source"
    write_tree(
        root,
        {
            "main.py": "import importlib\nname = 'plugins' + '.selected'\nprint(importlib.import_module(name).VALUE)\n",
            "plugins/__init__.py": "",
            "plugins/selected.py": "VALUE = 91\n",
            ".venv/broken.py": "if :\n",
            "build/broken.py": "if :\n",
        },
    )
    baseline = run([sys.executable, root / "main.py"])
    result = Calcium(backend="cpython", target="executable").build(
        root, output=tmp_path / "program"
    )
    assert result.success, result.diagnostics
    assert set(r["name"] for r in result.metadata["modules"]) == {
        "main",
        "plugins",
        "plugins.selected",
    }
    assert any(d.code == "C112" for d in result.diagnostics)
    assert_no_sources_needed(root, result.executable, baseline)


def test_namespace_and_additional_import_root(tmp_path):
    root, extra = tmp_path / "root", tmp_path / "extra"
    write_tree(
        root,
        {
            "main.py": "import ns.first\nimport ns.second\nprint(ns.first.VALUE, ns.second.VALUE, list(ns.__path__))\n",
            "ns/first.py": "VALUE = 1\n",
        },
    )
    write_tree(extra, {"ns/second.py": "VALUE = 2\n"})
    result = Calcium(backend="cpython", target="executable", include_paths=(extra,)).build(
        root / "main.py", output=tmp_path / "program"
    )
    assert result.success, result.diagnostics
    namespace = next(m for m in result.metadata["modules"] if m["name"] == "ns")
    assert namespace["namespace"] and namespace["c_file"] is None
    baseline = run(
        [
            sys.executable,
            "-c",
            f"import sys; sys.path[:0] = {[str(root), str(extra)]!r}; exec(compile(open({str(root / 'main.py')!r}).read(), {str(root / 'main.py')!r}, 'exec'))",
        ]
    )
    hidden = extra.with_name("extra-hidden")
    extra.rename(hidden)
    try:
        assert_no_sources_needed(root, result.executable, baseline)
    finally:
        hidden.rename(extra)


def test_strict_project_and_explicit_compatibility(tmp_path):
    root = tmp_path / "source"
    write_tree(
        root,
        {
            "main.py": "from pkg import model\nprint(model.VALUE)\n",
            "pkg/__init__.py": "",
            "pkg/model.py": "class Model:\n    pass\nVALUE = 9\n",
        },
    )
    output = tmp_path / "program"
    result = Calcium(backend="cpython", target="executable").build(root / "main.py", output=output)
    assert not result.success and not output.exists()
    assert any(d.code == "C101" and d.path.endswith("model.py") for d in result.diagnostics)
    result = Calcium(backend="cpython", target="executable", fallback=True).build(
        root / "main.py", output=output
    )
    assert result.success and result.metadata["mode"] == "mixed"
    assert result.fallbacks[0]["module"] == "pkg.model"
    baseline = run([sys.executable, root / "main.py"])
    assert_no_sources_needed(root, output, baseline)


def test_failed_import_rolls_back_cache_and_retries(tmp_path):
    root = tmp_path / "source"
    write_tree(
        root,
        {
            "main.py": """import sys, holder
for i in range(2):
    try:
        import broken
    except ZeroDivisionError:
        print('missing', 'broken' not in sys.modules)
print(holder.events)
""",
            "holder.py": "events = []\n",
            "broken.py": "import holder\nholder.events.append('attempt')\n1 / 0\n",
        },
    )
    baseline = run([sys.executable, root / "main.py"])
    result = Calcium(backend="cpython", target="executable", fallback=True).build(
        root / "main.py", output=tmp_path / "program"
    )
    assert result.success, result.diagnostics
    assert next(m for m in result.metadata["modules"] if m["name"] == "broken")["mode"] == "native"
    assert_no_sources_needed(root, result.executable, baseline)


def test_import_hooks_and_fromlist_values(tmp_path):
    root = tmp_path / "source"
    write_tree(
        root,
        {
            "main.py": """import builtins
original = builtins.__import__
def hook(name, globals=None, locals=None, fromlist=(), level=0):
    if name in ('pkg', 'helper', ''):
        print('hook', name, fromlist, level)
    return original(name, globals, locals, fromlist, level)
builtins.__import__ = hook
import helper
print(helper.get())
""",
            "helper.py": "from pkg import VALUE\ndef get():\n    from pkg import VALUE as local\n    return local + VALUE\n",
            "pkg/__init__.py": "VALUE = 11\n",
        },
    )
    baseline = run([sys.executable, root / "main.py"])
    result = Calcium(backend="cpython", target="executable", fallback=True).build(
        root / "main.py", output=tmp_path / "program"
    )
    assert result.success, result.diagnostics
    assert_no_sources_needed(root, result.executable, baseline)


def test_package_extension_without_sources(tmp_path):
    root = tmp_path / "source"
    write_tree(
        root,
        {
            "pkg/__init__.py": "from .arithmetic import add\n",
            "pkg/arithmetic.py": "def add(a, b):\n    return a + b\n",
        },
    )
    suffix = sysconfig.get_config_var("EXT_SUFFIX")
    output = tmp_path / ("pkg" + suffix)
    result = Calcium(backend="cpython", target="extension").build(root / "pkg", output=output)
    assert result.success, result.diagnostics
    baseline = run(
        [
            sys.executable,
            "-c",
            "import pkg, pkg.arithmetic; print(pkg.add(2, 3), pkg.arithmetic.add(5, 6))",
        ],
        cwd=root,
    )
    hidden = tmp_path / "source-hidden"
    root.rename(hidden)
    try:
        actual = run(
            [
                sys.executable,
                "-c",
                "import pkg, pkg.arithmetic; print(pkg.add(2, 3), pkg.arithmetic.add(5, 6))",
            ],
            cwd=tmp_path,
        )
        assert behavior(actual) == behavior(baseline), actual.stderr
    finally:
        hidden.rename(root)


def test_discovery_never_executes_and_dependency_diagnostics(tmp_path):
    root = tmp_path / "source"
    marker = tmp_path / "executed"
    write_tree(
        root,
        {
            "main.py": "import pkg.child\n",
            "pkg/__init__.py": f'open({str(marker)!r}, "w").write("bad")\n',
            "pkg/child.py": "VALUE = 1\n",
            "unimported.py": "if :\n",
        },
    )
    project = discover(root / "main.py")
    assert set(project.modules) == {"main", "pkg", "pkg.child"}
    assert not marker.exists()
    assert Calcium(backend="cpython").analyze(root / "main.py").success
    assert not marker.exists()
    (root / "pkg" / "child.py").write_text("if :\n")
    result = Calcium(backend="cpython").analyze(root / "main.py")
    assert not result.success and result.diagnostics[0].path.endswith("child.py")


def test_symlink_dependency_escapes_root(tmp_path):
    root = tmp_path / "source"
    write_tree(root, {"main.py": "import outside\n"})
    outside = tmp_path / "outside.py"
    outside.write_text("VALUE = 1\n")
    (root / "outside.py").symlink_to(outside)
    with pytest.raises(ValueError, match="escapes"):
        Calcium(backend="cpython").analyze(root / "main.py")


def test_project_emission_and_cli(tmp_path):
    root = tmp_path / "source"
    write_tree(
        root,
        {
            "pkg/__init__.py": "",
            "pkg/__main__.py": "from .child import VALUE\nprint(VALUE)\n",
            "pkg/child.py": "VALUE = 13\n",
        },
    )
    output = tmp_path / "generated"
    result = Calcium(backend="cpython", emit_ir=True, emit_ast=True).transpile(
        root / "pkg", output=output
    )
    assert result.success, result.diagnostics
    manifest = json.loads((output / "manifest.json").read_text())
    assert manifest["schema_version"] == 2 and len(manifest["modules"]) == 3
    assert all(
        (output / m["c_file"]).exists() and (output / m["ir_file"]).exists()
        for m in manifest["modules"]
    )
    assert Calcium(backend="cpython", emit_ir=True).transpile(root / "pkg", output=output).success
    cli = run(
        [
            sys.executable,
            "-m",
            "calcium",
            "run",
            "--module",
            "pkg",
            "--project-root",
            root,
            "--output",
            tmp_path / "program",
        ]
    )
    assert cli.returncode == 0 and cli.stdout == b"13\n", cli.stderr


def test_custom_finder_precedes_compiled_registry(tmp_path):
    root = tmp_path / "source"
    write_tree(
        root,
        {
            "main.py": """import sys, importlib.util
class Finder:
    def find_spec(self, fullname, path=None, target=None):
        if fullname == 'helper':
            return importlib.util.spec_from_loader(fullname, self)
    def create_module(self, spec):
        return None
    def exec_module(self, module):
        module.VALUE = 99
sys.meta_path.insert(0, Finder())
import helper
print(helper.VALUE)
""",
            "helper.py": "VALUE = 7\n",
        },
    )
    baseline = run([sys.executable, root / "main.py"])
    result = Calcium(backend="cpython", target="executable", fallback=True).build(
        root / "main.py", output=tmp_path / "program"
    )
    assert result.success, result.diagnostics
    assert_no_sources_needed(root, result.executable, baseline)


def test_dependency_fingerprint_changes_and_readable_artifacts(tmp_path):
    root = tmp_path / "source"
    write_tree(
        root, {"main.py": "from helper import VALUE\nprint(VALUE)\n", "helper.py": "VALUE = 1\n"}
    )
    first = Calcium(backend="cpython").transpile(root / "main.py", output=tmp_path / "first")
    second = Calcium(backend="cpython").transpile(root / "main.py", output=tmp_path / "second")
    assert first.success and second.success
    files_a = {p.name: p.read_bytes() for p in (tmp_path / "first").iterdir()}
    files_b = {p.name: p.read_bytes() for p in (tmp_path / "second").iterdir()}
    assert files_a == files_b
    (root / "helper.py").write_text("VALUE = 2\n")
    third = Calcium(backend="cpython").inspect(root / "main.py")
    assert third.success
    assert first.metadata["project_sha256"] != third.metadata["project_sha256"]
    assert first.metadata["source_sha256"] == third.metadata["source_sha256"]


def test_binary_dependency_is_not_mislabeled_as_native_source(tmp_path):
    root = tmp_path / "source"
    write_tree(root, {"main.py": "import external\n", "external.py": "VALUE = 7\n"})
    (root / ("external" + sysconfig.get_config_var("EXT_SUFFIX"))).write_bytes(b"not a real binary")
    result = Calcium(backend="cpython").inspect(root / "main.py")
    assert result.success
    assert result.analysis["external_imports"] == ["external"]
    assert [r["name"] for r in result.metadata["modules"]] == ["main"]


def test_explicit_module_entry_without_package_main(tmp_path):
    root = tmp_path / "source"
    write_tree(
        root,
        {
            "pkg/__init__.py": "",
            "pkg/cli.py": "from .shared import VALUE\nprint(__name__, __package__, VALUE)\n",
            "pkg/shared.py": "VALUE = 77\n",
        },
    )
    compiler = Calcium(backend="cpython", target="executable", project_root=root)
    with pytest.raises(ValueError, match="__main__"):
        compiler.build(root / "pkg", output=tmp_path / "wrong")
    result = compiler.build(module="pkg.cli", output=tmp_path / "program")
    assert result.success, result.diagnostics
    baseline = run([sys.executable, "-m", "pkg.cli"], cwd=root)
    assert_no_sources_needed(root, result.executable, baseline)


def test_include_all_cli_and_explicit_fallback_flag(tmp_path):
    root = tmp_path / "source"
    write_tree(
        root,
        {
            "main.py": "import importlib\nname = 'extra'\nprint(importlib.import_module(name).VALUE)\n",
            "extra.py": "VALUE = 55\n",
        },
    )
    output = tmp_path / "program"
    result = run(
        [
            sys.executable,
            "-m",
            "calcium",
            "build",
            root / "main.py",
            "--include-all",
            "--output",
            output,
        ]
    )
    assert result.returncode == 0, result.stderr
    baseline = run([sys.executable, root / "main.py"])
    assert_no_sources_needed(root, output, baseline)
    (root / "extra.py").write_text("class C:\n    pass\nVALUE = 55\n")
    strict = run(
        [sys.executable, "-m", "calcium", "check", root, "--no-fallback", "--format", "json"]
    )
    assert strict.returncode == 1 and not json.loads(strict.stdout)["success"]
    compat = run(
        [sys.executable, "-m", "calcium", "check", root, "--allow-fallback", "--format", "json"]
    )
    assert compat.returncode == 0 and json.loads(compat.stdout)["metadata"]["mode"] == "mixed"


def test_relative_literal_dynamic_import_module_entry(tmp_path):
    root = tmp_path / "source"
    write_tree(
        root,
        {
            "pkg/__init__.py": "",
            "pkg/cli.py": "import importlib\nprint(importlib.import_module('.child', __package__).VALUE)\n",
            "pkg/child.py": "VALUE = 63\n",
        },
    )
    result = Calcium(backend="cpython", target="executable", project_root=root).build(
        module="pkg.cli", output=tmp_path / "program"
    )
    assert result.success, result.diagnostics
    assert set(r["name"] for r in result.metadata["modules"]) == {"pkg", "pkg.cli", "pkg.child"}
    baseline = run([sys.executable, "-m", "pkg.cli"], cwd=root)
    assert_no_sources_needed(root, result.executable, baseline)


def test_module_benchmark_preserves_working_directory(tmp_path):
    root = tmp_path / "source"
    write_tree(root, {"pkg/__init__.py": "", "pkg/__main__.py": "import os\nprint(os.getcwd())\n"})
    result = run(
        [
            sys.executable,
            "-m",
            "calcium",
            "benchmark",
            "--module",
            "pkg",
            "--project-root",
            root,
            "--repeat",
            1,
            "--warmups",
            0,
            "--format",
            "json",
        ]
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["metadata"]["benchmark"]["speedup"] > 0
