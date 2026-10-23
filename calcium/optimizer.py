"""O1 removes unreachable blocks, without moving any observable operation."""

from .ir.nodes import Program
from .ir.validator import validate


def optimize(program: Program, level: int) -> list[dict]:
    validate(program)
    decisions = []
    if level and program.mode == "native":
        for fn in program.functions:
            blocks = {b.name: b for b in fn.blocks}
            seen, pending = set(), [fn.blocks[0].name]
            while pending:
                name = pending.pop()
                if name not in seen:
                    seen.add(name)
                    pending.extend(blocks[name].terminator.targets)
            removed = len(fn.blocks) - len(seen)
            fn.blocks[:] = [b for b in fn.blocks if b.name in seen]
            decisions.append(
                {
                    "pass": "unreachable-blocks",
                    "function": fn.name,
                    "removed": removed,
                    "assumption": "blocks with no reachable predecessor cannot execute",
                }
            )
    validate(program)
    return decisions
