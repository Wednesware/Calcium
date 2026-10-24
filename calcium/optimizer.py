"""Conservative passes; optimized arithmetic always retains a CPython slow path."""

from .ir.nodes import Instruction, Program, Terminator
from .ir.validator import validate


def _literal_truth(data):
    kind, value = data["type"], data["value"]
    if kind == "NoneType":
        return False
    if kind == "int":
        return bool(int(value))
    if kind == "float":
        return bool(float(value))
    return bool(value)


def optimize(program: Program, level: int) -> list[dict]:
    validate(program)
    decisions = []
    if level and program.mode in {"native", "mixed"}:
        for fn in program.functions:
            if level >= 2:
                count = 0
                for block in fn.blocks:
                    for op in block.instructions:
                        if op.op == "binary" and op.data in {"add", "subtract", "multiply"}:
                            op.op = "checked_binary"
                            count += 1
                decisions.append(
                    {
                        "pass": "checked-integer-arithmetic",
                        "function": fn.name,
                        "operations": count,
                        "assumption": "exact int and machine-range guards; overflow/subclasses use CPython",
                    }
                )
            if level >= 3:
                constants = {
                    op.result: op.data
                    for block in fn.blocks
                    for op in block.instructions
                    if op.op == "constant"
                }
                count = 0
                for block in fn.blocks:
                    term = block.terminator
                    if term.op == "branch" and term.args[0] in constants:
                        register = term.args[0]
                        chosen = term.targets[0 if _literal_truth(constants[register]) else 1]
                        block.instructions.append(Instruction("drop", [register], error=None))
                        block.terminator = Terminator("jump", targets=[chosen])
                        count += 1
                decisions.append(
                    {
                        "pass": "literal-branches",
                        "function": fn.name,
                        "folded": count,
                        "assumption": "immutable builtin constants have no observable truth callbacks",
                    }
                )
                validate(program)
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
    validate(program)
    return decisions
