import re

from ..models import CalciumInternalError
from .kernels import validate_kernel
from .nodes import Program

OPS = {
    "constant",
    "load",
    "store",
    "drop",
    "clear",
    "binary",
    "checked_binary",
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
    "import",
    "import_from",
    "join_string",
    "convert_string",
    "format",
    "unpack",
    "tuple_item",
    "delete",
    "delattr",
    "delitem",
    "assert_fail",
    "raise",
    "args_extend",
    "args_append",
    "kwargs_merge",
    "kwargs_set",
    "call_ex",
    "dict_merge",
    "to_tuple",
    "unpack_ex",
    "python_definition",
    "error_get",
    "error_set",
    "exception_take",
    "handled_get",
    "handled_set",
    "exception_raise",
    "exception_match",
    "special",
    "with_exit",
    "clear_binding",
    "set_update",
    "set_add",
}
PURE_CLEANUP = {
    "drop",
    "clear",
    "handled_set",
    "exception_raise",
    "clear_binding",
    "error_get",
    "error_set",
}


def validate(program: Program):
    if program.mode == "fallback":
        if program.functions or program.fallback_source is None:
            raise CalciumInternalError("invalid module fallback boundary")
        return
    if program.mode not in {"native", "mixed"} or not program.functions:
        raise CalciumInternalError("invalid program mode or missing module")
    symbols = {fn.symbol: fn for fn in program.functions}
    if len(symbols) != len(program.functions) or any(
        not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", symbol) for symbol in symbols
    ):
        raise CalciumInternalError("invalid or duplicate function symbol")
    if not program.functions[0].module or any(fn.module for fn in program.functions[1:]):
        raise CalciumInternalError("invalid module entry")
    for fn in program.functions:
        try:
            validate_kernel(fn)
        except (KeyError, TypeError, AttributeError, RecursionError) as exc:
            raise CalciumInternalError("malformed typed native kernel") from exc
        if not isinstance(fn.registers, int) or fn.registers < 0:
            raise CalciumInternalError("invalid register count")
        expected_parameters = (
            fn.positional + len(fn.kwonly) + int(fn.vararg is not None) + int(fn.kwarg is not None)
        )
        if (
            len(fn.parameters) != len(set(fn.parameters))
            or len(fn.parameters) != expected_parameters
            or not 0 <= fn.posonly <= fn.positional
        ):
            raise CalciumInternalError("invalid function signature")
        if fn.parameters[fn.positional : fn.positional + len(fn.kwonly)] != fn.kwonly:
            raise CalciumInternalError("invalid keyword-only parameter layout")
        blocks = {b.name: b for b in fn.blocks}
        if not blocks or len(blocks) != len(fn.blocks):
            raise CalciumInternalError("missing or duplicate blocks")
        if any(not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name) for name in blocks):
            raise CalciumInternalError("invalid control-flow label")
        if len(fn.locals) != len(set(fn.locals)) or not set(fn.parameters) <= set(fn.locals):
            raise CalciumInternalError("invalid local/parameter slots")
        if len(fn.cleanup_order) != len(fn.locals) or set(fn.cleanup_order) != set(fn.locals):
            raise CalciumInternalError("incomplete local ownership cleanup")
        if any(
            label not in blocks or not set(names) <= set(fn.locals)
            for label, names in fn.exception_handlers.items()
        ):
            raise CalciumInternalError("invalid exception landing pad")
        errors = {"error", *fn.exception_handlers}
        definitions = set()
        for block in fn.blocks:
            term = block.terminator
            if term is None or term.op not in {"return", "jump", "branch", "next", "error"}:
                raise CalciumInternalError("missing or invalid terminator")
            if any(t not in blocks for t in term.targets):
                raise CalciumInternalError("unknown control-flow target")
            expected = {
                "return": (1, 0),
                "jump": (0, 1),
                "branch": (1, 2),
                "next": (1, 2),
                "error": (0, 0),
            }[term.op]
            if (len(term.args), len(term.targets)) != expected:
                raise CalciumInternalError("terminator arity mismatch")
            if term.op in {"branch", "next", "error"} and term.error not in errors:
                raise CalciumInternalError("missing terminator exception edge")
            for op in block.instructions:
                if op.op not in OPS or (op.op not in PURE_CLEANUP and op.error not in errors):
                    raise CalciumInternalError("unknown operation or missing exception edge")
                arities = {
                    "constant": 0,
                    "load": 0,
                    "store": 1,
                    "drop": 1,
                    "clear": 0,
                    "binary": 2,
                    "checked_binary": 2,
                    "inplace": 2,
                    "unary": 1,
                    "compare": 2,
                    "attribute": 1,
                    "getitem": 2,
                    "slice": 3,
                    "setattr": 2,
                    "setitem": 3,
                    "iter": 1,
                    "import": 0,
                    "import_from": 1,
                    "convert_string": 1,
                    "format": 2,
                    "unpack": 1,
                    "tuple_item": 1,
                    "delete": 0,
                    "delattr": 1,
                    "delitem": 2,
                    "args_extend": 2,
                    "args_append": 2,
                    "kwargs_merge": 2,
                    "kwargs_set": 2,
                    "call_ex": 3,
                    "dict_merge": 2,
                    "to_tuple": 1,
                    "unpack_ex": 1,
                    "python_definition": 0,
                    "error_get": 0,
                    "error_set": 1,
                    "exception_take": 0,
                    "handled_get": 0,
                    "handled_set": 1,
                    "exception_raise": 1,
                    "exception_match": 2,
                    "special": 1,
                    "with_exit": 2,
                    "clear_binding": 0,
                    "set_update": 2,
                    "set_add": 2,
                }
                if op.op in arities and len(op.args) != arities[op.op]:
                    raise CalciumInternalError("instruction arity mismatch")
                if op.op in {"binary", "inplace", "checked_binary"}:
                    operators = (
                        {"add", "subtract", "multiply"}
                        if op.op == "checked_binary"
                        else {
                            "add",
                            "subtract",
                            "multiply",
                            "true_divide",
                            "floor_divide",
                            "remainder",
                            "power",
                            "lshift",
                            "rshift",
                            "and",
                            "or",
                            "xor",
                        }
                    )
                    if op.data not in operators:
                        raise CalciumInternalError("unknown arithmetic operation")
                if op.op == "convert_string" and op.data not in (115, 114, 97):
                    raise CalciumInternalError("invalid string conversion")
                if op.op == "python_definition" and (
                    not fn.module or program.mode != "mixed" or not isinstance(op.data, str)
                ):
                    raise CalciumInternalError("invalid definition fallback boundary")
                if op.op == "unpack" and (not isinstance(op.data, int) or op.data < 0):
                    raise CalciumInternalError("invalid unpack count")
                if op.op == "unpack_ex" and any(
                    not isinstance(op.data[key], int) or op.data[key] < 0
                    for key in ("before", "after")
                ):
                    raise CalciumInternalError("invalid starred unpack counts")
                has_result = op.op not in {
                    "store",
                    "drop",
                    "clear",
                    "setattr",
                    "setitem",
                    "delete",
                    "delattr",
                    "delitem",
                    "assert_fail",
                    "raise",
                    "args_extend",
                    "args_append",
                    "kwargs_merge",
                    "kwargs_set",
                    "dict_merge",
                    "python_definition",
                    "handled_set",
                    "error_set",
                    "exception_raise",
                    "clear_binding",
                    "set_update",
                    "set_add",
                }
                if op.op == "raise" and len(op.args) not in (0, 1, 2):
                    raise CalciumInternalError("raise argument layout mismatch")
                if op.op == "assert_fail" and len(op.args) not in (0, 1):
                    raise CalciumInternalError("assertion argument layout mismatch")
                if has_result != (op.result is not None):
                    raise CalciumInternalError("invalid instruction result")
                if op.op == "clear" and op.data not in fn.locals:
                    raise CalciumInternalError("unknown cleanup slot")
                if op.op == "call" and len(op.args) != 1 + op.data["positional"] + len(
                    op.data["keywords"]
                ):
                    raise CalciumInternalError("call argument layout mismatch")
                if op.op == "function" and len(op.args) != op.data["posdefaults"] + len(
                    op.data["kwdefaults"]
                ):
                    raise CalciumInternalError("function default layout mismatch")
                if op.op == "function":
                    definition = symbols.get(op.data["symbol"])
                    if definition is None or definition.module:
                        raise CalciumInternalError("unknown function definition")
                    if not 0 <= op.data["posdefaults"] <= definition.positional or not set(
                        op.data["kwdefaults"]
                    ) <= set(definition.kwonly):
                        raise CalciumInternalError("invalid function defaults")
            for result in [op.result for op in block.instructions] + [term.result]:
                if result is not None:
                    if result in definitions or not 0 <= result < fn.registers:
                        raise CalciumInternalError("invalid or repeated register definition")
                    definitions.add(result)
        producers = {
            op.result: op
            for block in fn.blocks
            for op in block.instructions
            if op.result is not None
        }
        for block in fn.blocks:
            for op in block.instructions:
                if op.op == "tuple_item":
                    producer = producers.get(op.args[0])
                    if producer is None or producer.op not in {"unpack", "unpack_ex"}:
                        raise CalciumInternalError("tuple access requires verified unpack result")
                    size = (
                        producer.data
                        if producer.op == "unpack"
                        else producer.data["before"] + producer.data["after"] + 1
                    )
                    if not isinstance(op.data, int) or not 0 <= op.data < size:
                        raise CalciumInternalError("tuple access is out of bounds")
        # Ownership/dominance fixed point. Every reachable predecessor must agree
        # on live register ownership; mutable slots carry merged values instead.
        incoming = {name: frozenset() for name in [fn.blocks[0].name, *fn.exception_handlers]}
        pending = list(incoming)
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
