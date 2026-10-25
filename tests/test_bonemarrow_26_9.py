"""26.9: real native coroutine scheduling, CLI parsing, and HTTP serving."""

import os
import random
import select
import shutil
import socket
import subprocess
import sys
from pathlib import Path

import pytest

from calcium import Calcium

FIXTURES = Path(__file__).parent / "fixtures" / "bonemarrow269"


def compare(tmp_path, source, level=3, arguments=()):
    source_file = tmp_path / "program.py"
    source_file.write_text(source)
    python_work = tmp_path / "python"
    native_work = tmp_path / "native"
    python_work.mkdir()
    native_work.mkdir()
    expected = subprocess.run(
        [sys.executable, str(source_file), *arguments],
        cwd=python_work,
        capture_output=True,
        timeout=30,
    )
    result = Calcium(target="executable", optimization=level).build(
        source_file, output=tmp_path / "program"
    )
    assert result.success, result.diagnostics
    actual = subprocess.run(
        [str(result.executable), *arguments],
        cwd=native_work,
        capture_output=True,
        timeout=30,
        env={**os.environ, "PATH": "", "PYTHONHOME": "/missing", "PYTHONPATH": "/missing"},
    )
    assert (actual.returncode, actual.stdout) == (expected.returncode, expected.stdout), (
        actual,
        expected,
    )
    assert actual.returncode == 0, actual.stderr
    return result


@pytest.mark.parametrize("level", [0, 3])
@pytest.mark.parametrize("fixture", sorted(FIXTURES.glob("*.py")))
def test_native_parity(tmp_path, fixture, level):
    compare(tmp_path, fixture.read_text(), level)


def test_arbitrary_precision_bitwise(tmp_path):
    rng = random.Random(269)
    pairs = [
        (
            rng.getrandbits(rng.randrange(1, 1500)) * (-1 if rng.randrange(2) else 1),
            rng.getrandbits(rng.randrange(1, 1500)) * (-1 if rng.randrange(2) else 1),
        )
        for _ in range(50)
    ]
    source = f"pairs={pairs!r}\n"
    source += """for a,b in pairs:
    print(a|b,a&b,a^b,~a)
    for count in [0,1,29,30,31,63,130,1000]: print(a<<count,a>>count)
print(True|False,True&False,True^True)
try: print(1<<-1)
except ValueError: print('negative shift')
try: print(1.5|2)
except TypeError: print('invalid operand')
"""
    compare(tmp_path, source)


def test_real_process_arguments(tmp_path):
    compare(
        tmp_path,
        """import sys,argparse
p=argparse.ArgumentParser(prog='tool')
p.add_argument('files',nargs='*')
p.add_argument('--port',type=int,default=80)
a=p.parse_intermixed_args()
print(sys.argv[1:],a.files,a.port)
""",
        arguments=["first", "--port", "123", "second"],
    )


@pytest.mark.parametrize("compiler", [c for c in ("gcc", "clang") if shutil.which(c)])
def test_async_sanitizers(tmp_path, compiler):
    source = (
        (FIXTURES / "asyncio.py").read_text()
        + """
async def many():
    keep=[]
    for i in range(4000):
        async def inner():
            await asyncio.sleep(0)
            return i
        keep.append(await inner())
        cycle=[]
        cycle.append(cycle)
    print(len(keep),keep[-1])
asyncio.run(many())
"""
    )
    source_file = tmp_path / "source.py"
    source_file.write_text(source)
    expected = subprocess.run([sys.executable, str(source_file)], capture_output=True, timeout=30)
    emitted = tmp_path / "c"
    result = Calcium().transpile(source_file, output=emitted)
    assert result.success, result.diagnostics
    binary = tmp_path / "sanitized"
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-g",
            "-O1",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer",
            "-fno-pie",
            "-no-pie",
            "-pthread",
            "-I",
            str(emitted),
            *map(str, emitted.glob("*.c")),
            "-lm",
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
        timeout=120,
    )
    run = subprocess.run(
        [str(binary)],
        capture_output=True,
        timeout=30,
        env={
            **os.environ,
            "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1",
        },
    )
    assert run.returncode == 0, run.stderr.decode(errors="replace")
    assert run.stderr == b"", run.stderr
    assert run.stdout == expected.stdout, (run, expected)


def test_http_real_concurrent_requests(tmp_path):
    web = tmp_path / "web"
    web.mkdir()
    (web / "index.html").write_text("native home")
    (web / "hé.txt").write_text("hé😀")
    source = tmp_path / "server.py"
    source.write_text("""from http.server import SimpleHTTPRequestHandler,ThreadingHTTPServer
from functools import partial
server=ThreadingHTTPServer(('127.0.0.1',0),partial(SimpleHTTPRequestHandler,directory='web'))
server.timeout=0.001
server.handle_request()
print(server.server_port,flush=True)
try: server.serve_forever(poll_interval=0.01)
except KeyboardInterrupt: pass
finally: server.server_close()
""")
    result = Calcium(target="executable", optimization=3).build(source, output=tmp_path / "server")
    assert result.success, result.diagnostics
    process = subprocess.Popen(
        [str(result.executable)],
        cwd=tmp_path,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, "PATH": "", "PYTHONHOME": "/missing", "PYTHONPATH": "/missing"},
    )
    slow = None
    try:
        assert select.select([process.stdout], [], [], 10)[0], "server did not start"
        line = process.stdout.readline()
        assert line, process.stderr.read().decode()
        port = int(line)
        slow = socket.create_connection(("127.0.0.1", port), timeout=2)
        slow.sendall(b"GET / HTTP/1.0\r\n")
        for path, method, status, body in [
            ("/", "GET", 200, b"native home"),
            ("/h%C3%A9.txt", "GET", 200, "hé😀".encode()),
            ("/h%C3%A9.txt", "HEAD", 200, b""),
            ("/missing", "GET", 404, None),
        ]:
            with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
                connection.sendall(f"{method} {path} HTTP/1.0\r\nHost: localhost\r\n\r\n".encode())
                response = b""
                while chunk := connection.recv(65536):
                    response += chunk
            headers, payload = response.split(b"\r\n\r\n", 1)
            assert f" {status} ".encode() in headers
            if body is not None:
                assert payload == body
        slow.close()
        slow = None
        process.send_signal(2)
        stdout, stderr = process.communicate(timeout=10)
        assert process.returncode == 0, (stdout, stderr)
    finally:
        if slow:
            slow.close()
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=10)
