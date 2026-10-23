"""Filesystem-only project discovery: never imports the analyzed project."""

import ast
from dataclasses import dataclass, field
from importlib.machinery import EXTENSION_SUFFIXES
from pathlib import Path

from .loader import Source, load

EXCLUDED = {".git", ".venv", "venv", "__pycache__", "build", "dist", "node_modules", ".tox", ".nox"}


@dataclass
class Module:
    name: str
    path: Path
    root: Path
    package: bool = False
    namespace: bool = False
    source: Source | None = None
    tree: ast.Module | None = None
    namespace_paths: list[Path] = field(default_factory=list)

    @property
    def directory(self):
        return self.path if self.namespace else self.path.parent


@dataclass
class Project:
    entry: Module
    roots: list[Path]
    modules: dict[str, Module] = field(default_factory=dict)
    imports: dict[str, list[str]] = field(default_factory=dict)
    external: set[str] = field(default_factory=set)
    dynamic: list[dict] = field(default_factory=list)
    module_entry: bool = False
    directory_input: bool = False


def infer_root(path: Path) -> Path:
    root = path.parent
    while (root / "__init__.py").is_file():
        root = root.parent
    return root


def _inside(path, root):
    if not path.resolve().is_relative_to(root):
        raise ValueError(f"project source symlink escapes declared root: {path}")
    return path.resolve()


def resolve(name: str, roots: list[Path]) -> Module | None:
    if not name or any(not part.isidentifier() for part in name.split(".")):
        return None
    # Resolve one path component at a time so a concrete parent package excludes
    # unrelated search roots, just as ordinary package __path__ lookup does.
    locations = list(roots)
    result = None
    for index, part in enumerate(name.split(".")):
        qualified = ".".join(name.split(".")[: index + 1])
        namespaces = []
        result = None
        for location in locations:
            root = next(r for r in roots if location.is_relative_to(r))
            folder = location / part
            initializer = folder / "__init__.py"
            file = location / (part + ".py")
            if any((folder / ("__init__" + suffix)).is_file() for suffix in EXTENSION_SUFFIXES):
                return None  # Binary packages must remain runtime dependencies.
            if initializer.is_file():
                result = Module(qualified, _inside(initializer, root), root, package=True)
                locations = [folder.resolve()]
                break
            if any((location / (part + suffix)).is_file() for suffix in EXTENSION_SUFFIXES):
                return None
            if file.is_file():
                result = Module(qualified, _inside(file, root), root)
                locations = []
                break
            if (folder / "__init__.pyc").is_file():
                return None
            if folder.is_dir():
                namespaces.append((_inside(folder, root), root))
        if result is None and namespaces:
            directory, root = namespaces[0]
            result = Module(
                qualified,
                directory,
                root,
                package=True,
                namespace=True,
                namespace_paths=[p for p, _ in namespaces],
            )
            locations = [directory for directory, _ in namespaces]
        if result is None or (index < len(name.split(".")) - 1 and not result.package):
            return None
    return result


def _absolute(module: Module, name: str | None, level: int) -> str | None:
    if not level:
        return name or ""
    package = module.name if module.package else module.name.rpartition(".")[0]
    parts = package.split(".") if package else []
    if level > len(parts):
        return None
    base = parts[: len(parts) - level + 1]
    return ".".join(base + ([name] if name else []))


def discover(
    source=None, *, module=None, root=None, include_paths=(), include_all=False, library=False
) -> Project:
    if source is not None and module is not None:
        raise ValueError("choose a source path or --module, not both")
    if source is None and module is None:
        raise ValueError("a source path or --module is required")
    directory_input = False
    module_entry = module is not None
    if module is not None:
        primary = Path(root or ".").resolve()
    else:
        path = Path(source).resolve()
        directory_input = path.is_dir()
        if directory_input:
            primary = (
                Path(root).resolve()
                if root
                else (path.parent if (path / "__init__.py").is_file() else path)
            )
        else:
            primary = Path(root).resolve() if root else infer_root(path)
    roots = list(dict.fromkeys([primary, *(Path(p).resolve() for p in include_paths)]))
    if not all(p.is_dir() for p in roots):
        raise ValueError("all project/import roots must be existing directories")
    if module is not None:
        entry = resolve(module, roots)
        if entry is None:
            raise ValueError(f"module {module!r} not found in the declared project roots")
        if entry.package and not library:
            entry = resolve(module + ".__main__", roots) or entry
    elif directory_input:
        if (path / "__init__.py").is_file():
            name = ".".join(path.relative_to(primary).parts)
            entry = (
                resolve(name, roots)
                if library
                else resolve(name + ".__main__", roots) or resolve(name, roots)
            )
            module_entry = True
        else:
            file = path / "__main__.py" if (path / "__main__.py").is_file() else path / "main.py"
            if not file.is_file():
                raise ValueError(
                    "project directory needs main.py or __main__.py; alternatively use --module"
                )
            entry = Module(file.stem, file, primary)
    else:
        if not path.is_relative_to(primary):
            raise ValueError("entry source must be inside the project root")
        relative = path.relative_to(primary).with_suffix("")
        parts = relative.parts[:-1] if path.name == "__init__.py" else relative.parts
        entry = Module(".".join(parts), path, primary, package=path.name == "__init__.py")
    if entry is None or entry.namespace:
        raise ValueError("an executable entry must be a Python module or regular package")
    project = Project(entry, roots, module_entry=module_entry, directory_input=directory_input)
    pending = [entry]

    def enqueue(name):
        found = resolve(name, roots)
        if found is None:
            return False
        for i in range(1, len(name.split("."))):
            parent = resolve(".".join(name.split(".")[:i]), roots)
            if parent is not None and parent.name not in project.modules:
                pending.append(parent)
        if found.name not in project.modules:
            pending.append(found)
        return True

    # Module execution initializes its enclosing packages before running main.
    if module_entry:
        for i in range(1, len(entry.name.split("."))):
            enqueue(".".join(entry.name.split(".")[:i]))
    if include_all or directory_input:
        scan = path if directory_input else primary
        for file in sorted(scan.rglob("*.py")):
            rel = file.relative_to(scan)
            if any(part in EXCLUDED or part.startswith(".") for part in rel.parts):
                continue
            relative = file.relative_to(primary).with_suffix("")
            parts = relative.parts[:-1] if file.name == "__init__.py" else relative.parts
            if parts:
                enqueue(".".join(parts))
    while pending:
        item = pending.pop()
        if item.name in project.modules:
            continue
        project.modules[item.name] = item
        project.imports[item.name] = []
        if item.namespace:
            continue
        item.source = load(item.path)
        item.tree = ast.parse(item.source.text, filename=str(item.path))
        for node in ast.walk(item.tree):
            names = []
            candidates = []
            if isinstance(node, ast.Import):
                names = [alias.name for alias in node.names]
            elif isinstance(node, ast.ImportFrom):
                base = _absolute(item, node.module, node.level)
                if base:
                    names = [base]
                    candidates = [
                        base + "." + alias.name for alias in node.names if alias.name != "*"
                    ]
            elif isinstance(node, ast.Call) and (
                isinstance(node.func, ast.Name)
                and node.func.id == "__import__"
                or isinstance(node.func, ast.Attribute)
                and node.func.attr == "import_module"
            ):
                target = (
                    node.args[0].value
                    if node.args
                    and isinstance(node.args[0], ast.Constant)
                    and isinstance(node.args[0].value, str)
                    else None
                )
                if (
                    target is not None
                    and target.startswith(".")
                    and isinstance(node.func, ast.Attribute)
                ):
                    anchor = (
                        node.args[1]
                        if len(node.args) > 1
                        else next((k.value for k in node.keywords if k.arg == "package"), None)
                    )
                    package = None
                    if isinstance(anchor, ast.Constant) and isinstance(anchor.value, str):
                        package = anchor.value
                    elif isinstance(anchor, ast.Name) and anchor.id == "__package__":
                        package = item.name if item.package else item.name.rpartition(".")[0]
                    level = len(target) - len(target.lstrip("."))
                    if package and level <= len(package.split(".")):
                        base = package.split(".")[: len(package.split(".")) - level + 1]
                        target = ".".join(base + ([target[level:]] if target[level:] else []))
                    else:
                        target = None
                if target is not None and not target.startswith("."):
                    names = [target]
                else:
                    project.dynamic.append(
                        {
                            "module": item.name,
                            "line": node.lineno,
                            "reason": "dynamic import name is not statically known; use a directory input or --include-all",
                        }
                    )
            for name in names:
                project.imports[item.name].append(name)
                if not enqueue(name):
                    project.external.add(name)
            for name in candidates:
                if enqueue(name):
                    project.imports[item.name].append(name)
    project.entry = project.modules[entry.name]
    return project
