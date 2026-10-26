"""Native library API availability; these names never route into CPython."""

MODULES = frozenset(
    {
        "subprocess",
        "zipfile",
        "re",
        "urllib",
        "urllib.error",
        "urllib.request",
        "traceback",
        "importlib",
        "importlib.util",
        "threading",
        "sys",
        "contextlib",
        "argparse",
        "asyncio",
        "http",
        "http.server",
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

LIBRARIES.update(
    {
        "re": ("libpcre2-8", "CA_WITH_PCRE2"),
        "zipfile": ("libarchive", "CA_WITH_ARCHIVE"),
        "urllib.request": ("libcurl", "CA_WITH_CURL"),
    }
)
