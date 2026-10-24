"""Compare C-lowered functions using Python-defined protocol fixtures."""

import subprocess
import sys
import sysconfig

import pytest

from calcium import Calcium

SOURCE = """def use(manager):
    with manager as value:
        print('body', value)
        if value == 'raise':
            raise ValueError('body failure')
        return value

def create(key, value, values):
    print({key(1):value(1), **{}, key(2):value(2)})
    print({key(3), *values, key(4)})
    return [x for x in values if x]

def caught(error, expected):
    try:
        raise error
    except expected as caught:
        return str(caught)
"""

RUNNER = """import importlib.util,sys
spec=importlib.util.spec_from_file_location('protocols',sys.argv[1])
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
class Special:
    def __init__(self,name): self.name=name
    def __get__(self,obj,owner):
        print('bind',self.name)
        if self.name=='enter': return lambda: obj.value
        return lambda *exc: print('exit', exc[0].__name__ if exc[0] else None) or obj.suppress
class Manager:
    __enter__=Special('enter')
    __exit__=Special('exit')
    def __init__(self,value,suppress=False): self.value,self.suppress=value,suppress
for value in ['ok','raise']:
    for suppress in [False,True]:
        try: print('result',m.use(Manager(value,suppress)))
        except Exception as exc: print('failure',type(exc).__name__,str(exc))
class Key:
    def __init__(self,value): self.value=value
    def __hash__(self): print('hash',self.value);return self.value
    def __repr__(self): return str(self.value)
def key(n): print('key',n);return Key(n)
def value(n): print('value',n);return n
print(m.create(key,value,[5,6]))
for expected in [(ValueError,TypeError),(),(ValueError,1),((ValueError,),)]:
    try: print(m.caught(ValueError('error'),expected))
    except Exception as exc: print(type(exc).__name__,str(exc))
"""


@pytest.mark.parametrize("level", [0, 3])
def test_native_protocol_ordering(tmp_path, level):
    source = tmp_path / "protocols.py"
    source.write_text(SOURCE)
    output = tmp_path / ("protocols" + sysconfig.get_config_var("EXT_SUFFIX"))
    result = Calcium(target="extension", optimization=level, fallback=False).build(
        source, output=output
    )
    assert result.success, result.diagnostics
    original = subprocess.run(
        [sys.executable, "-c", RUNNER, str(source)], capture_output=True, timeout=10
    )
    compiled = subprocess.run(
        [sys.executable, "-c", RUNNER, str(output)], capture_output=True, timeout=10
    )
    assert (compiled.returncode, compiled.stdout, compiled.stderr) == (
        original.returncode,
        original.stdout,
        original.stderr,
    )
