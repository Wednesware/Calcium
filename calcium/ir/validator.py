from ..models import CalciumInternalError
from .nodes import Program

OPS = {
    "constant",
    "load",
    "store",
    "drop",
    "clear",
    "binary",
    "inplace",
    "unary",
    "compare",
    "attribute",
    "getitem",
    "slice",
    "call",
    "container",
    "setattr",
    "setitem",
    "function",
    "iter",
}
PURE_CLEANUP = {"drop", "clear"}


def validate(program: Program):
    if program.mode == "fallback":
        if program.functions or program.fallback_source is None:
            raise CalciumInternalError("invalid module fallback boundary")
        return
    for fn in program.functions:
        blocks = {b.name: b for b in fn.blocks}
        if not blocks or len(blocks) != len(fn.blocks):
            raise CalciumInternalError("missing or duplicate blocks")
        if len(fn.locals) != len(set(fn.locals)) or not set(fn.parameters) <= set(fn.locals):
            raise CalciumInternalError("invalid local/parameter slots")
        if len(fn.cleanup_order) != len(fn.locals) or set(fn.cleanup_order) != set(fn.locals):
            raise CalciumInternalError("incomplete local ownership cleanup")
        definitions = set()
        for block in fn.blocks:
            term = block.terminator
            if term is None or term.op not in {"return", "jump", "branch", "next"}:
                raise CalciumInternalError("missing or invalid terminator")
            if any(t not in blocks for t in term.targets):
                raise CalciumInternalError("unknown control-flow target")
            expected = {"return": (1, 0), "jump": (0, 1), "branch": (1, 2), "next": (1, 2)}[term.op]
            if (len(term.args), len(term.targets)) != expected:
                raise CalciumInternalError("terminator arity mismatch")
            if term.op in {"branch", "next"} and term.error != "error":
                raise CalciumInternalError("missing terminator exception edge")
            for op in block.instructions:
                if op.op not in OPS or (op.op not in PURE_CLEANUP and op.error != "error"):
                    raise CalciumInternalError("unknown operation or missing exception edge")
                arities = {
                    "constant": 0,
                    "load": 0,
                    "store": 1,
                    "drop": 1,
                    "clear": 0,
                    "binary": 2,
                    "inplace": 2,
                    "unary": 1,
                    "compare": 2,
                    "attribute": 1,
                    "getitem": 2,
                    "slice": 3,
                    "setattr": 2,
                    "setitem": 3,
                    "function": 0,
                    "iter": 1,
                }
                if op.op in arities and len(op.args) != arities[op.op]:
                    raise CalciumInternalError("instruction arity mismatch")
                has_result = op.op not in {"store", "drop", "clear", "setattr", "setitem"}
                if has_result != (op.result is not None):
                    raise CalciumInternalError("invalid instruction result")
                if op.op == "clear" and op.data not in fn.locals:
                    raise CalciumInternalError("unknown cleanup slot")
                if op.op == "call" and len(op.args) != 1 + op.data["positional"] + len(
                    op.data["keywords"]
                ):
                    raise CalciumInternalError("call argument layout mismatch")
            for result in [op.result for op in block.instructions] + [term.result]:
                if result is not None:
                    if result in definitions or not 0 <= result < fn.registers:
                        raise CalciumInternalError("invalid or repeated register definition")
                    definitions.add(result)
        # Ownership/dominance fixed point. Every reachable predecessor must agree
        # on live register ownership; mutable slots carry merged values instead.
        incoming = {fn.blocks[0].name: frozenset()}
        pending = [fn.blocks[0].name]
        while pending:
            name = pending.pop()
            block = blocks[name]
            live = set(incoming[name])
            for op in block.instructions:
                if any(a not in live for a in op.args):
                    raise CalciumInternalError(f"use of unowned register in {fn.name}/{name}")
                if op.op == "drop":
                    live.remove(op.args[0])
                if op.result is not None:
                    live.add(op.result)
            term = block.terminator
            if any(a not in live for a in term.args):
                raise CalciumInternalError("terminator uses unowned register")
            if term.args:
                live.remove(term.args[0])
            for i, target in enumerate(term.targets):
                edge = live | ({term.result} if term.op == "next" and i == 0 else set())
                if target in incoming:
                    if incoming[target] != frozenset(edge):
                        raise CalciumInternalError(f"ownership mismatch at join {target}")
                else:
                    incoming[target] = frozenset(edge)
                    pending.append(target)
