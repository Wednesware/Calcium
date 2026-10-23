"""Use CPython's own nonexecuting constant-folding decisions for this target."""

import ast
import dis

MISSING = object()


def folded(node):
    allowed = (ast.Constant, ast.Tuple, ast.UnaryOp, ast.BinOp, ast.Load, ast.operator, ast.unaryop)
    if not all(isinstance(item, allowed) for item in ast.walk(node)):
        return MISSING
    code = compile(ast.Expression(node), "<constant>", "eval", dont_inherit=True, optimize=0)
    instructions = [
        i for i in dis.get_instructions(code) if i.opname not in {"RESUME", "CACHE", "NOP"}
    ]
    if len(instructions) == 1 and instructions[0].opname == "RETURN_CONST":
        return instructions[0].argval
    if len(instructions) == 2 and [i.opname for i in instructions] == [
        "LOAD_CONST",
        "RETURN_VALUE",
    ]:
        return instructions[0].argval
    return MISSING


def encode(value):
    if isinstance(value, tuple):
        data = [encode(v) for v in value]
    elif isinstance(value, bytes):
        data = value.hex()
    elif isinstance(value, (str, bool, type(None))):
        data = value
    else:
        data = str(value)
    return {"type": type(value).__name__, "value": data}
