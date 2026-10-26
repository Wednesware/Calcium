import subprocess

r = subprocess.run(
    ["/bin/sh", "-c", "printf hello; printf error >&2"], capture_output=True, text=True
)
print(r.returncode, r.stdout, r.stderr)
r.check_returncode()
r = subprocess.run(["/bin/cat"], input="hé\n".encode(), capture_output=True)
print(r.stdout, r.stderr)
r = subprocess.run(
    ["sh", "-c", "printf %s $VALUE"],
    env={"VALUE": "native", "PATH": "/bin"},
    capture_output=True,
    text=True,
)
print(r.stdout)
print(subprocess.run("printf shell", shell=True, capture_output=True, text=True).stdout)
print(subprocess.check_output(["/bin/printf", "ok"], text=True))
print(subprocess.check_call(["/bin/true"]))
try:
    subprocess.run(
        ["/bin/sh", "-c", "printf out; printf err >&2; exit 7"], capture_output=True, check=True
    )
except subprocess.CalledProcessError as e:
    print(e.returncode, e.output, e.stdout, e.stderr)
p = subprocess.Popen(
    ["/bin/cat"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
)
print(p.communicate(b"bytes"), p.returncode, p.wait(), p.poll())
p = subprocess.Popen(["/bin/sleep", "0.08"])
try:
    p.wait(timeout=0.001)
except subprocess.TimeoutExpired as e:
    print("timeout", e.timeout)
print(p.wait())
try:
    subprocess.run(["/bin/sleep", "1"], timeout=0.001)
except subprocess.TimeoutExpired:
    print("run timeout")
try:
    subprocess.run(["/missing/calcium-command"])
except FileNotFoundError:
    print("missing")
p = subprocess.Popen(["/bin/cat"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
p.stdin.write(b"manual pipe")
p.stdin.close()
print(p.stdout.read(), p.wait())
