"""Native 26.11 regex, process, thread, traceback and registry APIs."""

import importlib.util
import re
import subprocess
import threading
import traceback

print(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", "Calcium 26.11 native_tools"))
print(subprocess.check_output(["/bin/printf", "native process"], text=True))
values = []
thread = threading.Thread(target=lambda: values.append("native thread"))
thread.start()
thread.join()
print(values)
try:
    raise ValueError("native exception")
except ValueError as error:
    print("".join(traceback.format_exception_only(error)), end="")
print(importlib.util.find_spec("re") is not None)
