"""Differential native libraries, local network transfers and lifetime checks."""

import contextlib
import http.server
import os
import shutil
import ssl
import subprocess
import sys
import threading
from pathlib import Path

import pytest

from calcium import Calcium

FIXTURES = Path(__file__).parent / "fixtures" / "bonemarrow2611"


def build(source, tmp_path, level=3, compiler=None, **kwargs):
    result = Calcium(target="executable", optimization=level, compiler=compiler, **kwargs).build(
        source, output=tmp_path / "program"
    )
    assert result.success, result.diagnostics
    return result


def run_pair(source, tmp_path, level=3, compiler=None, **kwargs):
    expected_dir = tmp_path / "python"
    native_dir = tmp_path / "native"
    expected_dir.mkdir()
    native_dir.mkdir()
    expected = subprocess.run(
        [sys.executable, str(source)], cwd=expected_dir, capture_output=True, timeout=30
    )
    result = build(source, tmp_path, level, compiler, **kwargs)
    actual = subprocess.run(
        [str(result.executable)],
        cwd=native_dir,
        capture_output=True,
        timeout=30,
        env={**os.environ, "PATH": "", "PYTHONHOME": "/missing", "PYTHONPATH": "/missing"},
    )
    assert actual.returncode == 0, actual.stderr.decode(errors="replace")
    assert (actual.stdout, actual.stderr) == (expected.stdout, expected.stderr), (actual, expected)
    assert expected.returncode == 0, expected.stderr
    return result


@pytest.mark.parametrize("level", [0, 3])
@pytest.mark.parametrize("source", sorted(FIXTURES.glob("*.py")), ids=lambda p: p.stem)
def test_library_parity(tmp_path, source, level):
    run_pair(source.resolve(), tmp_path, level)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/data")
            self.end_headers()
            return
        body = b"native\x00HTTP\n"
        self.send_response(404 if self.path == "/missing" else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


@contextlib.contextmanager
def server(tls=None):
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    if tls:
        httpd.socket = tls.wrap_socket(httpd.socket, server_side=True)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"{'https' if tls else 'http'}://127.0.0.1:{httpd.server_port}"
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join()


def test_url_transfers_redirects_errors_and_callbacks(tmp_path):
    source = tmp_path / "network.py"
    with server() as base:
        source.write_text(f"""from urllib import request
urlopen=request.urlopen
urlretrieve=request.urlretrieve
from urllib.error import HTTPError
from pathlib import Path
base={base!r}
with urlopen(base+'/redirect',timeout=5) as response:
    print(response.status,response.getcode(),response.geturl().endswith('/data'))
    print(response.getheader('content-type'))
    print(response.read(3),response.read())
progress=[]
def report(block,size,total):progress.append(block)
filename,headers=urlretrieve(base+'/data','download',report)
print(filename,Path(filename).read_bytes(),progress[0],progress[-1]>0)
with urlopen(base+'/echo',data=b'POST body') as response:print(response.read())
try:urlopen(base+'/missing')
except HTTPError as error:print(error.code,isinstance(error, OSError))
""")
        result = run_pair(source, tmp_path)
    assert {"libcurl"} <= set(result.runtime_dependencies)


def test_https_keeps_certificate_verification(tmp_path):
    certificate, key = tmp_path / "cert.pem", tmp_path / "key.pem"
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-days",
            "1",
            "-subj",
            "/CN=localhost",
            "-keyout",
            str(key),
            "-out",
            str(certificate),
        ],
        check=True,
        capture_output=True,
        timeout=15,
    )
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(certificate, key)
    with server(tls) as base:
        source = tmp_path / "tls.py"
        source.write_text(
            "from urllib.request import urlopen\nfrom urllib.error import URLError\n"
            f"try:urlopen({base!r},timeout=3)\nexcept URLError:print('certificate rejected')\n"
        )
        run_pair(source, tmp_path)


def test_compiled_imports_specs_and_source_free_execution(tmp_path):
    package = tmp_path / "pkg"
    package.mkdir()
    (package / "__init__.py").write_text("value=42\n")
    (package / "child.py").write_text("value=43\n")
    source = tmp_path / "main.py"
    source.write_text("""import importlib.util
from pathlib import Path
name='pkg'
spec=importlib.util.spec_from_file_location(name,str(Path(__file__).parent/'pkg'/'__init__.py'),submodule_search_locations=[])
m=importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
print(m.value,spec.name)
print(importlib.import_module(name).value)
print(importlib.import_module('.child',name).value)
print(importlib.util.find_spec('missing_calcium_module') is None)
try:importlib.import_module('missing_calcium_module')
except ModuleNotFoundError:print('missing')
""")
    run_pair(source, tmp_path, include_all=True)
    shutil.rmtree(package)
    actual = subprocess.run(
        [str(tmp_path / "program")],
        capture_output=True,
        timeout=10,
        env={**os.environ, "PATH": "", "PYTHONHOME": "/missing", "PYTHONPATH": "/missing"},
    )
    assert actual.returncode == 0, actual.stderr
    assert actual.stdout == b"42 pkg\n42\n43\nTrue\nmissing\n"


def test_traceback_native_source_locations_and_json_streams(tmp_path):
    source = tmp_path / "trace.py"
    source.write_text("""import traceback,json
from pathlib import Path
def fail():raise ValueError('native error')
try:fail()
except ValueError as error:
    text=traceback.format_exc()
    print('trace.py' in text,'fail' in text,'ValueError: native error' in text)
    print(traceback.format_exception_only(error))
with open('values.json','w') as stream:json.dump({'native':[1,True]},stream)
with open('values.json') as stream:print(json.load(stream))
""")
    run_pair(source, tmp_path)


def test_zip_extraction_does_not_follow_existing_symlinks(tmp_path):
    import zipfile

    outside = tmp_path / "outside"
    outside.mkdir()
    output = tmp_path / "output"
    output.mkdir()
    (output / "link").symlink_to(outside, target_is_directory=True)
    with zipfile.ZipFile(tmp_path / "unsafe.zip", "w") as archive:
        archive.writestr("link/escape", "bad")
    source = tmp_path / "extract.py"
    source.write_text(f"""import zipfile
try:
    with zipfile.ZipFile({str(tmp_path / "unsafe.zip")!r}) as archive:archive.extractall({str(output)!r})
except OSError:print('symlink rejected')
""")
    result = build(source, tmp_path)
    actual = subprocess.run([str(result.executable)], capture_output=True, timeout=10)
    assert actual.returncode == 0, actual.stderr
    assert actual.stdout == b"symlink rejected\n"
    assert not (outside / "escape").exists()


@pytest.mark.parametrize("compiler", [n for n in ("gcc", "clang") if shutil.which(n)])
def test_native_library_sanitizers(tmp_path, compiler):
    wrapper = tmp_path / "compiler"
    wrapper.write_text(
        f"#!/bin/sh\nexec {compiler} -g -fsanitize=address,undefined "
        '-fno-sanitize-recover=all -fno-omit-frame-pointer -fno-pie -no-pie "$@"\n'
    )
    wrapper.chmod(0o755)
    source = tmp_path / "sanitized.py"
    source.write_text(
        "\n".join(p.read_text() for p in sorted(FIXTURES.glob("*.py")))
        + """\nvalues=[]
lock=threading.Lock()
def allocate(index):
    roots=[str(index)]
    for i in range(1500):
        cycle=[]
        cycle.append(cycle)
        roots.append(str(i))
        if i%50==0:time.sleep(0)
    with lock:values.append((index,len(roots),roots[0]))
threads=[threading.Thread(target=allocate,args=(i,)) for i in range(4)]
for thread in threads:thread.start()
for thread in threads:thread.join()
print(sorted(values))
"""
    )
    run_pair(source, tmp_path, level=1, compiler=str(wrapper), cache=False)


def test_uncompiled_source_and_isolated_reexecution_are_explicit_errors(tmp_path):
    package = tmp_path / "pkg"
    package.mkdir()
    (package / "__init__.py").write_text("value=42\n")
    source = tmp_path / "main.py"
    source.write_text("""import importlib.util
from pathlib import Path
try:importlib.util.spec_from_file_location('later',str(Path(__file__).parent/'later.py'))
except ImportError:print('rebuild required')
spec=importlib.util.spec_from_file_location('pkg',str(Path(__file__).parent/'pkg'/'__init__.py'))
m=importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
try:importlib.util.module_from_spec(spec)
except ImportError:print('isolated reexecution unsupported')
name='re'
print(importlib.util.find_spec(name) is None)
try:importlib.import_module(name)
except ModuleNotFoundError:print('unlinked stdlib rejected')
""")
    result = build(source, tmp_path, include_all=True)
    (tmp_path / "later.py").write_text("raise RuntimeError('must never execute')\n")
    actual = subprocess.run([str(result.executable)], capture_output=True, timeout=10)
    assert actual.returncode == 0, actual.stderr
    assert (
        actual.stdout
        == b"rebuild required\nisolated reexecution unsupported\nTrue\nunlinked stdlib rejected\n"
    )
