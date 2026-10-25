"""A small, serializable control-flow IR; the C backend never sees an AST."""

from dataclasses import asdict, dataclass, field
from typing import Any


@dataclass
class Instruction:
    op: str
    args: list[int] = field(default_factory=list)
    result: int | None = None
    data: Any = None
    line: int = 1
    error: str | None = "error"


@dataclass
class Terminator:
    op: str
    args: list[int] = field(default_factory=list)
    targets: list[str] = field(default_factory=list)
    result: int | None = None
    error: str | None = None


@dataclass
class Block:
    name: str
    instructions: list[Instruction] = field(default_factory=list)
    terminator: Terminator | None = None


@dataclass
class Function:
    name: str
    symbol: str
    parameters: list[str]
    locals: list[str]
    blocks: list[Block] = field(default_factory=list)
    registers: int = 0
    doc: str | None = None
    module: bool = False
    cleanup_order: list[str] = field(default_factory=list)
    posonly: int = 0
    positional: int = 0
    kwonly: list[str] = field(default_factory=list)
    vararg: str | None = None
    kwarg: str | None = None
    exception_handlers: dict[str, list[str]] = field(default_factory=dict)
    kernel: dict | None = None
    native: dict | None = None


@dataclass
class Program:
    functions: list[Function]
    mode: str = "native"
    fallback_source: str | None = None
    schema_version: int = 2

    def to_dict(self):
        return asdict(self)
