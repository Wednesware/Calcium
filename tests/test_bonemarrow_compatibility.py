"""26.8 independent runtime compatibility: compare output and error behavior."""

import os
import subprocess
import sys
from pathlib import Path

import pytest

from calcium import Calcium

PROGRAMS = [
    """class Base:
    value=4
    def __init__(self,x=2): self.x=x
    def result(self): return self.x+self.value
class Derived(Base):
    value=7
    def __str__(self): return f'Derived:{self.x}'
a=Derived(3)
print(a.result(),a,Derived.__name__,Derived.__module__)
print(isinstance(a,Base),isinstance(a,Derived),issubclass(Derived,Base))
a.value=10
print(a.result(),Base.value,Derived.value)
""",
    """def factory(start):
    def change(amount=1):
        nonlocal start
        start+=amount
        return start
    return change
one=factory(1)
two=factory(10)
print(one(),two(),one(4),two())
def counter():
    values=[]
    return lambda value: values.append(value) or values
f=counter()
print(f(1),f(2))
""",
    """events=[]
def deco(tag):
    events.append(tag)
    def apply(fn):
        events.append('apply'+tag)
        def wrapper(*args,**kwargs): return tag+fn(*args,**kwargs)
        return wrapper
    return apply
@deco('a')
@deco('b')
def f(x='c'): return x
print(events,f())
""",
    """class C:
    def __init__(self,x): self.x=x
    @classmethod
    def create(cls,x=3): return cls(x)
    @staticmethod
    def sum(a,b): return a+b
    @property
    def doubled(self): return self.x*2
c=C.create()
print(c.x,c.doubled,C.sum(2,3),c.sum(4,5),c.create(9).x)
""",
    """def f():
    try:
        raise ValueError('bad')
    except (OSError,ValueError) as error:
        print(str(error),repr(error),error.args)
        return 7
    finally: print('finally')
print(f())
try:
    f()
    print(error)
except NameError:
    print('binding cleared')
""",
    """class Conflict(ValueError): pass
try:
    try: raise Conflict('first')
    except ValueError:
        try: raise TypeError('second') from ValueError('cause')
        except TypeError as error:
            print(str(error.__cause__))
        raise
except Conflict as error:
    print(str(error),isinstance(error,ValueError))
""",
    """events=[]
class Context:
    def __init__(self,suppress): self.suppress=suppress
    def __enter__(self): events.append('enter');return 3
    def __exit__(self,kind,error,tb):
        events.append('exit')
        print(kind.__name__ if kind is not None else None,str(error))
        return self.suppress
with Context(True) as value:
    print(value)
    raise ValueError('suppressed')
try:
    with Context(False): raise TypeError('propagated')
except TypeError: print('caught')
print(events)
""",
    """values=[(i,j) for i in range(5) if i>1 for j in range(3) if j!=1]
print(values,{i:i*i for i in range(4)}, {i for i in range(3)}=={0,1,2})
print(f'{"hé😀"!r}',f'{"hé😀"!a}',f'{7:04d}',f'{1.25:.2f}',f'{"é":>4}')
print(str(2**80),int('  -1_234 '),int('ff',16),float('1_2.5'),bool([]),repr(None))
""",
    """for a in [-100,-2,0,2,100]:
    for b in [-7,3]: print(a//b,a%b,a/b)
print((10**60)//(-3),(10**60)%(-3),2**100)
""",
    """def outer(value):
    def first(): return second()
    def second(): return value
    return first
print(outer(9)())
functions=[]
for i in range(4):
    def f(x=i): return x
    functions.append(f)
print([f() for f in functions])
""",
    """try:
    assert False,'problem'
except AssertionError as error:
    print(str(error))
try:
    x={}
    x['missing']
except KeyError:
    print('key error')
""",
]


def compare(tmp_path, source, level=3):
    file = tmp_path / "program.py"
    file.write_text(source)
    python_work = tmp_path / "python-work"
    native_work = tmp_path / "native-work"
    python_work.mkdir()
    native_work.mkdir()
    expected = subprocess.run(
        [sys.executable, str(file)], cwd=python_work, capture_output=True, timeout=30
    )
    result = Calcium(target="executable", optimization=level).build(
        file, output=tmp_path / "program"
    )
    assert result.success, result.diagnostics
    actual = subprocess.run(
        [result.executable],
        cwd=native_work,
        capture_output=True,
        timeout=30,
        env={**os.environ, "PATH": "", "PYTHONHOME": "/missing", "PYTHONPATH": "/missing"},
    )
    assert actual.stdout == expected.stdout, (actual, expected)
    assert actual.returncode == expected.returncode, (actual, expected)
    if actual.returncode:
        if expected.stderr.startswith(b"Traceback"):
            assert actual.stderr.split(b":", 1)[0] + b":" in expected.stderr
        else:
            assert actual.stderr == expected.stderr
    return result


@pytest.mark.parametrize("level", [0, 3])
@pytest.mark.parametrize("source", PROGRAMS)
def test_language(tmp_path, source, level):
    compare(tmp_path, source, level)


def test_division_rounding_bigints_and_subnormals(tmp_path):
    import random

    rng = random.Random(268)
    cases = [
        (10**500, 10**500),
        (1, 2**1075),
        (3, 2**1075),
        (1, 2**1074),
        (-1, 2**1076),
        (2**1024 - 2**970, 1),
        (2**1024 - 2**971, 1),
    ]
    cases += [
        (
            rng.getrandbits(rng.randrange(1, 5000)) * (1 if rng.randrange(2) else -1),
            rng.getrandbits(rng.randrange(1, 5000)) + 1,
        )
        for _ in range(100)
    ]
    code = ""
    for a, b in cases:
        code += (
            f'try:\n    print({hex(a)}/{hex(b)})\nexcept OverflowError:\n    print("overflow")\n'
        )
        code += f"print({hex(a)}//{hex(b)},{hex(a)}%{hex(b)})\n"
    compare(tmp_path, code)


LIBRARY_PROGRAMS = sorted((Path(__file__).parent / "fixtures" / "bonemarrow").glob("*.py"))


@pytest.mark.parametrize("level", [0, 3])
@pytest.mark.parametrize("source", LIBRARY_PROGRAMS)
def test_native_library_parity(tmp_path, source, level):
    result = compare(tmp_path, source.read_text(), level)
    assert result.metadata["backend"] == "native"


def test_native_resources_and_closure_gc_sanitizers(tmp_path):
    import shlex
    import shutil

    source = tmp_path / "stress.py"
    library = Path(__file__).parent / "fixtures" / "bonemarrow" / "libraries.py"
    source.write_text(
        library.read_text()
        + """
def factory(value):
    state=[value]
    def update():
        state[0]+=1
        return state[0]
    return update
saved=[factory(i) for i in range(20)]
for i in range(12000):
    temporary=factory(i)
    temporary()
    cycle=[]
    cycle.append(cycle)
    value=json.loads(json.dumps({'value':[i,'hé😀']}))
print([f() for f in saved])
class Truth:
    def __init__(self,result): self.result=result
    def __bool__(self):
        for i in range(1000): values=[i]*10
        return self.result
class Item:
    def __init__(self,value): self.value=value
    def __lt__(self,other): return Truth(self.value<other.value)
print([x.value for x in sorted([Item(3),Item(1),Item(2)])])
"""
    )
    expected_work = tmp_path / "python"
    expected_work.mkdir()
    expected = subprocess.run(
        [sys.executable, str(source)], cwd=expected_work, capture_output=True, timeout=60
    )
    assert expected.returncode == 0, expected.stderr
    emitted = tmp_path / "emitted"
    result = Calcium().transpile(source, output=emitted)
    assert result.success, result.diagnostics
    libraries = shlex.split(
        subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "openssl", "sqlite3"], text=True
        )
    )
    for compiler in [c for c in ("gcc", "clang") if shutil.which(c)]:
        binary = tmp_path / compiler
        subprocess.run(
            [
                compiler,
                "-std=c11",
                "-O1",
                "-g",
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer",
                "-fno-pie",
                "-no-pie",
                "-DCA_WITH_OPENSSL=1",
                "-DCA_WITH_SQLITE=1",
                "-I",
                str(emitted),
                *map(str, emitted.glob("*.c")),
                *libraries,
                "-lm",
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
            timeout=120,
        )
        work = tmp_path / (compiler + "-work")
        work.mkdir()
        actual = subprocess.run(
            [str(binary)],
            cwd=work,
            capture_output=True,
            timeout=60,
            env={
                **os.environ,
                "PATH": "",
                "PYTHONHOME": "/missing",
                "PYTHONPATH": "/missing",
                "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                "UBSAN_OPTIONS": "halt_on_error=1",
            },
        )
        assert actual.returncode == 0, actual.stderr.decode(errors="replace")
        assert actual.stderr == b"", actual.stderr
        assert actual.stdout == expected.stdout


@pytest.mark.parametrize("code", ["None", "0", "42", "-2", "'goodbye'", "(1,2)"])
def test_system_exit(tmp_path, code):
    compare(tmp_path, f"raise SystemExit({code})\n")
