"""Public results are independent of terminal presentation and compiler internals."""

from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class Diagnostic:
    code: str
    message: str
    path: str = ""
    line: int = 1
    column: int = 1
    severity: str = "error"
    stage: str = "frontend"


@dataclass(frozen=True)
class Artifact:
    kind: str
    path: str
    sha256: str


@dataclass
class Result:
    success: bool = False
    diagnostics: list[Diagnostic] = field(default_factory=list)
    artifacts: list[Artifact] = field(default_factory=list)
    optimizations: list[dict[str, Any]] = field(default_factory=list)
    fallbacks: list[dict[str, Any]] = field(default_factory=list)
    runtime_dependencies: list[str] = field(default_factory=list)
    metadata: dict[str, Any] = field(default_factory=dict)
    timings: dict[str, float] = field(default_factory=dict)
    analysis: dict[str, Any] = field(default_factory=dict)
    ir: dict[str, Any] | None = None
    optimization_report: dict[str, Any] = field(default_factory=dict)
    schema_version: int = 1

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @property
    def executable(self) -> Path | None:
        return next((Path(a.path) for a in self.artifacts if a.kind == "executable"), None)


class CalciumInternalError(RuntimeError):
    """A compiler invariant failed. Invalid C must never be published."""
