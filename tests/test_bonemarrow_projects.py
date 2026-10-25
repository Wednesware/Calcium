"""Native project semantics and CLI defaults, differential against CPython."""

import json
import os
import shutil
import subprocess
import sys

import pytest

from calcium import Calcium, __version__


def project(root, files):
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    return root


def execute(command, **options):
    return subprocess.run(list(map(str, command)), capture_output=True, timeout=30, **options)


def compare(root, source="main.py", *, module=None, level=3):
    output = root.parent / (root.name + "-program")
    result = Calcium(target="executable", optimization=level, project_root=root, keep_c=True).build(
        None if module else root / source, module=module, output=output
    )
    assert result.success, result.diagnostics
    expected = execute([sys.executable, *(["-m", module] if module else [root / source])], cwd=root)
    actual = execute(
        [output],
        cwd=root,
        env={**os.environ, "PATH": "", "PYTHONHOME": "/missing", "PYTHONPATH": "/missing"},
    )
    assert actual.stdout == expected.stdout
    assert actual.returncode == expected.returncode, (actual.stderr, expected.stderr)
    if expected.returncode:
        kind = actual.stderr.split(b":", 1)[0]
        assert kind + b":" in expected.stderr
    assert "libpython" not in subprocess.check_output(["ldd", output], text=True)
    for path in output.with_name(output.name + ".calcium").glob("*.c"):
        assert "Python.h" not in path.read_text() and "Py_Initialize" not in path.read_text()
    return result, output


FILES = {
    "main.py": """import pkg.deep.math as math
from pkg import deep
from pkg.deep.math import square as sq
import pkg.deep.math
print(sq(12), math.VALUE, deep.math.VALUE)
math.VALUE=20
print(math.square(3),math.VALUE)
from pkg import calls
print(calls.recurse(8),__name__,__package__)
""",
    "pkg/__init__.py": "print('package')\nVALUE=9\n",
    "pkg/deep/__init__.py": "print('nested')\n",
    "pkg/deep/math.py": "print('math')\nVALUE=4\ndef square(x: int)->int:\n    global VALUE\n    VALUE+=1\n    return x*x\n",
    "pkg/calls.py": "from . import other\ndef recurse(n):\n    if n<1: return 1\n    return other.recurse(n-1)+1\n",
    "pkg/other.py": "from . import calls\ndef recurse(n):\n    if n<1: return 2\n    return calls.recurse(n-1)+2\n",
}


@pytest.mark.parametrize("level", [0, 1, 2, 3])
def test_native_nested_imports_and_calls(tmp_path, level):
    root = project(tmp_path / "project", FILES)
    result, output = compare(root, level=level)
    assert {m["name"] for m in result.metadata["modules"]} == {
        "pkg",
        "pkg.deep",
        "pkg.deep.math",
        "pkg.calls",
        "pkg.other",
        "__main__",
    }
    shutil.rmtree(root)
    assert execute([output], env={"PATH": "", "PYTHONHOME": "/missing"}).returncode == 0


@pytest.mark.parametrize(
    "files",
    [
        {
            "main.py": "import a\nimport a\nfrom a import b\nprint(a.x,b.y)\n",
            "a.py": "print('a start')\nx=3\nimport b\nprint('a end')\n",
            "b.py": "print('b')\nimport a\ny=a.x+2\n",
        },
        {
            "main.py": "import a\n",
            "a.py": "from b import value\nvalue=3\n",
            "b.py": "from a import value\n",
        },
        {
            "main.py": "from pkg import child\nprint(child.x)\n",
            "pkg/__init__.py": "from . import child\nprint('initialized')\n",
            "pkg/child.py": "x=3\nprint('child')\n",
        },
        {
            "main.py": "import p.child as c\nprint(c.x)\n",
            "p/__init__.py": "from . import child\n",
            "p/child.py": "x=2\n",
        },
        {"main.py": "from pkg import missing\n", "pkg/__init__.py": "print('start')\n"},
        {"main.py": "print('start')\nimport missing_package\n"},
        {"main.py": "from . import other\n", "other.py": "x=1\n"},
        {"main.py": "import ns.child\nprint(ns.child.value)\n", "ns/child.py": "value=7\n"},
        {
            "main.py": "print(__name__)\nif __name__=='__main__':\n    import main\n    print(main.__name__)\n"
        },
        {
            "main.py": "import a\nimport b\nprint(a.x,b.x,a.f(),b.f())\n",
            "a.py": "x=1\ndef f(): return x\n",
            "b.py": "x=2\ndef f(): return x\n",
        },
        {
            "main.py": "import a.b.c\nfrom a.b.c import value\nprint(value)\n",
            "a/__init__.py": "print('a')\n",
            "a/b/__init__.py": "print('b')\n",
            "a/b/c.py": "print('c')\nvalue=7\n",
        },
    ],
)
def test_import_order_cache_and_errors(tmp_path, files):
    compare(project(tmp_path / "project", files))


@pytest.mark.parametrize("module", ["pkg", "pkg.cli"])
def test_module_entry(tmp_path, module):
    files = {
        "pkg/__init__.py": "print('pkg')\nVALUE=42\n",
        "pkg/__main__.py": "from . import VALUE\nprint(__name__,__package__,VALUE)\n",
        "pkg/cli.py": "from . import VALUE\nprint(__name__,__package__,VALUE)\n",
    }
    root = project(tmp_path / "project", files)
    compare(root, module=module)
    if module == "pkg":
        result = Calcium(target="executable").build(root / "pkg", output=tmp_path / "dir-program")
        assert result.success, result.diagnostics
        assert (
            execute([result.executable]).stdout
            == execute([sys.executable, "-m", "pkg"], cwd=root).stdout
        )


def test_include_paths_and_output_ownership(tmp_path):
    root = project(
        tmp_path / "project", {"main.py": "from mathutils import square\nprint(square(12))\n"}
    )
    extra = project(tmp_path / "extra", {"mathutils.py": "def square(x): return x*x\n"})
    compiler = Calcium(target="executable", include_paths=(extra,))
    result = compiler.build(root / "main.py", output=tmp_path / "program")
    assert result.success, result.diagnostics
    assert execute([result.executable]).stdout == b"144\n"
    with pytest.raises(ValueError, match="input|sources"):
        compiler.build(root / "main.py", output=extra / "mathutils.py")


@pytest.mark.parametrize(
    "flags",
    [
        [],
        ["--use", "bmrw"],
        ["--bmrw"],
        ["--backend", "native"],
        ["--backend", "bmrw"],
        ["--bmrw", "--use", "bmrw"],
    ],
)
def test_cli_default_aliases_logging_and_info(tmp_path, flags):
    source = tmp_path / "main.py"
    source.write_text("print('application')")
    command = [sys.executable, "-m", "calcium", source, *flags, "-o", tmp_path / "program"]
    quiet = execute(command)
    assert quiet.returncode == 0, quiet.stderr
    assert quiet.stderr == b"" and quiet.stdout.strip().endswith(b"program")
    logged = execute([*command, "-O3", "-v", "--fancy", "--format", "json"])
    assert logged.returncode == 0, logged.stderr
    assert json.loads(logged.stdout)["metadata"]["backend"] == "native"
    assert b"using Bonemarrow (bmrw)" in logged.stderr
    assert b"optimizing at O3" in logged.stderr
    assert b"linking executable" in logged.stderr
    assert b"awakening Bonemarrow..." in logged.stderr
    app = execute([tmp_path / "program"])
    assert app.stdout == b"application\n" and app.stderr == b""
    info = execute([sys.executable, "-m", "calcium", "--info", "--format", "json"])
    data = json.loads(info.stdout)
    assert data["bonemarrow_version"] == __version__ and data["backend"] == "bmrw"


@pytest.mark.parametrize(
    "flags",
    [
        ["--bmrw", "--cpython"],
        ["--use", "bmrw", "--backend", "cpython"],
        ["--backend", "native", "--backend", "cpython"],
    ],
)
def test_cli_conflicts(tmp_path, flags):
    result = execute([sys.executable, "-m", "calcium", "--check", "missing.py", *flags])
    assert result.returncode == 2 and b"conflicting backend" in result.stderr


CONTAINERS = r"""def f(values=[], *, item=2):
    values += [item]
    return values
print(f(),f(item=3))
def g(a, /, b=4, *, c=5):
    return a+b+c
print(g(2),g(3,c=2,b=1))
a=[1,2,3]
b=a
a += (4,5)
a *= 2
print(a,b)
a[0]=a
print(a)
d={'a':1, True:2, 1:3}
d['z']=[1,2]
print(d, len(d), d[True], list(d))
print({1,2}==set([2,1,1]), {1}<{1,2}, 2 in {2:4}, 4 not in [1])
print((1,'hé😀',None),tuple([True,2]),list(range(5)))
for item in [None, 3, 'a']:
    print(item)
print('😀aé'[-1], '😀aé'[::-1], [1,2,3,4][::-2], (1,2,3)[:2])
print(len('😀aé'),len(range(7,1,-2)),2.0 in range(5), 2.5 in range(5))
print([1,2]<[1,3],(1,)==(True,),[1]!=(1,))
print(None is None,1 is not None,True is True)
"""


@pytest.mark.parametrize("level", [0, 1, 2, 3])
def test_containers_defaults_and_slices(tmp_path, level):
    code = (
        CONTAINERS
        + """
def collect(a, /, b=2, *items, c=3, **options):
    return a,b,items,c,options
print(collect(1,2,3,4,c=5,a=8,x=9))
print(collect(*[1],**{'c':6}))
values=[]
append=values.append
append(1)
values.extend(range(3))
a,b=values[:2]
print(values.pop(),values,a,b)
d={'a':1}
print(d.get('missing',7),d.copy())
d.update({'b':2})
print(d)
s=set()
s.add(2)
print(2 in s)
print([1,*range(3)],(*[1],2), {1,*[2,3]}=={1,2,3})
print({'a':1,**{'b':2}})
print(['\\x85','\\u200b','\\u00a0','\\n',"a'b",'a"b'])
"""
    )
    compare(project(tmp_path / "project", {"main.py": code}), level=level)


@pytest.mark.parametrize(
    "code,kind",
    [
        ("print([1][3])", "IndexError"),
        ("print({1:2}[3])", "KeyError"),
        ("print({[]:3})", "TypeError"),
        ("print([1][::0])", "ValueError"),
        ("def f(*,x): pass\nf()", "TypeError"),
        ("def f(*,x=2): pass\nf(3)", "TypeError"),
        ("def f(x=1): pass\nf(2,x=3)", "TypeError"),
        ("d={1:2}\nfor x in d:\n    d[3]=4", "RuntimeError"),
    ],
)
def test_collection_and_binding_errors(tmp_path, code, kind):
    result, _ = compare(project(tmp_path / "project", {"main.py": code}))
    assert result.success


def test_module_container_gc_sanitizers(tmp_path):
    root = project(
        tmp_path / "project",
        {
            "main.py": "import state\nfor i in range(30000):\n    state.fill(i)\nprint(state.data[0][0],len(state.data))\n",
            "state.py": "data=[[0]]\ndata[0][0]=data\ndef fill(i):\n    global data\n    data=[[i]]\n    return [i]*10\n",
        },
    )
    compare(root)
    emitted = tmp_path / "generated"
    result = Calcium().transpile(root / "main.py", output=emitted)
    assert result.success, result.diagnostics
    for compiler in [c for c in ["gcc", "clang"] if shutil.which(c)]:
        binary = tmp_path / (compiler + "-asan")
        subprocess.run(
            [
                compiler,
                "-std=c11",
                "-g",
                "-O1",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-fno-pie",
                "-no-pie",
                "-I",
                str(emitted),
                *map(str, emitted.glob("*.c")),
                "-lm",
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
        )
        run = execute(
            [binary],
            env={
                **os.environ,
                "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                "UBSAN_OPTIONS": "halt_on_error=1",
            },
        )
        assert run.returncode == 0 and run.stdout == b"29999 1\n", run.stderr


def test_native_project_cache_recompiles_changed_module(tmp_path):
    root = project(
        tmp_path / "project", {"main.py": "import worker\nprint(worker.x)\n", "worker.py": "x=2\n"}
    )
    compiler = Calcium(target="executable", optimization=2, cache_dir=tmp_path / "cache")
    first = compiler.build(root / "main.py", output=tmp_path / "program")
    second = compiler.build(root / "main.py", output=tmp_path / "program")
    assert first.success and second.success
    assert all(not row["hit"] for row in first.metadata["toolchain"]["cache"])
    assert all(row["hit"] for row in second.metadata["toolchain"]["cache"])
    (root / "worker.py").write_text("x=4\n")
    third = compiler.build(root / "main.py", output=tmp_path / "program")
    assert third.success and execute([third.executable]).stdout == b"4\n"
    rows = third.metadata["toolchain"]["cache"]
    assert sum(not row["hit"] for row in rows) == 1
    # Corruption is a validated miss, never a loaded untrusted object file.
    key = rows[0]["key"]
    entry = tmp_path / "cache" / key[:2] / key / "object.o"
    entry.write_bytes(b"corrupt")
    fourth = compiler.build(root / "main.py", output=tmp_path / "program")
    assert fourth.success and any(not row["hit"] for row in fourth.metadata["toolchain"]["cache"])
    debug = Calcium(target="executable", debug=True, cache_dir=tmp_path / "cache").build(
        root / "main.py", output=tmp_path / "debug"
    )
    assert debug.success and not any(row["hit"] for row in debug.metadata["toolchain"]["cache"])


@pytest.mark.parametrize(
    "code",
    [
        "from worker import *",
        "class C(metaclass=type): pass",
        "try:\n    pass\nexcept* ValueError: pass",
        "async def f(): return 1",
        "import ctypes",
        "x=[1,2]\nx[:]=[3]",
        "def f(*args: print('side')): pass",
    ],
)
def test_native_blockers_are_explicit(tmp_path, code):
    root = project(tmp_path / "project", {"main.py": code, "worker.py": "x=1\n"})
    result = Calcium(fallback=True).inspect(root / "main.py")
    assert not result.success and not result.fallbacks
    errors = [d for d in result.diagnostics if d.severity == "error"]
    assert errors and all(d.code == "C160" and "--cpython" in d.message for d in errors)


def test_deep_mutable_graph_and_bound_method_survive_gc(tmp_path):
    code = """data=[]
for i in range(5000):
    data=[data]
method=[].append
for i in range(30000):
    temporary=[i]*20
method(4)
count=0
while data:
    count+=1
    data=data[0]
print(count)
"""
    compare(project(tmp_path / "project", {"main.py": code}))


def test_main_import_uses_active_namespace(tmp_path):
    root = project(
        tmp_path / "project",
        {
            "__main__.py": "def identity(x=3): return x\nimport __main__\nprint(__main__.identity(),__main__.__name__)\n"
        },
    )
    compare(root, source="__main__.py")


def test_function_defaults_cross_module_gc_and_evaluation_order(tmp_path):
    root = project(
        tmp_path / "project",
        {
            "main.py": "from worker import record\nfor i in range(30000):\n    temporary=[i]*10\nprint(record(),record(value=4))\n",
            "worker.py": "def default():\n    print('default')\n    return []\ndef record(values=default(), *, value=3):\n    values.append(value)\n    return values\n",
        },
    )
    compare(root)


def test_nan_identity_in_collections_and_bound_methods(tmp_path):
    code = """x=1e400-1e400
y=1e400-1e400
print(x==x,[x]==[x],[x]==[y],x in [x],y in [x])
d={x:3}
print(d[x],x in d,y in d,len({x,x,y}))
values=[]
method=values.append
mapping={method:7}
print(mapping[method],values.append==values.append)
"""
    compare(project(tmp_path / "project", {"main.py": code}))


def test_native_warning_is_diagnostic_not_raw_stderr(tmp_path):
    source = tmp_path / "warning.py"
    source.write_text("print('\\q')\n")
    result = execute([sys.executable, "-m", "calcium", source, "--check", "--format", "json"])
    assert result.returncode == 0 and result.stderr == b""
    diagnostics = json.loads(result.stdout)["diagnostics"]
    assert any(d["code"] == "C105" for d in diagnostics)
