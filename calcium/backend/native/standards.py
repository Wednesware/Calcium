"""Native library API availability; these names never route into CPython."""

MODULES = frozenset(
    {
        "os",
        "os.path",
        "pathlib",
        "json",
        "hashlib",
        "sqlite3",
        "tempfile",
        "shutil",
        "time",
        "uuid",
        "secrets",
        "datetime",
        "dataclasses",
        "functools",
    }
)
LIBRARIES = {"hashlib": ("openssl", "CA_WITH_OPENSSL"), "sqlite3": ("sqlite3", "CA_WITH_SQLITE")}
