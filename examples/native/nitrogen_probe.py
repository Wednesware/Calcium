"""Read-only Nitrogen import/helper probe; pass --include-path /path/to/discribe."""

from discribe import nitrogen

print(
    nitrogen.parsepub("ca"),
    nitrogen._sanitize_python_name(" hello:there? "),
    nitrogen._publication_leaf("calcium", "26.11"),
)
print(nitrogen.InstallResult("ok", ["native"]))
