"""Prove pure integer regions before allowing replay on a checked guard miss.

No heap writes, unknown calls, global numeric loads, or introspection are allowed.
The generic CIR remains the slow path. This is an explicit typed kernel IR, not C.
"""

import ast
import struct

from ..frontend.constants import MISSING, folded


class Ineligible(Exception):
    pass


def discover_kernel(node, locals_):
    bits = struct.calcsize("l") * 8
    minimum, maximum = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    names = list(locals_)
    parameters = [a.arg for a in node.args.posonlyargs + node.args.args + node.args.kwonlyargs]
    if node.args.vararg or node.args.kwarg or len(names) > 32:
        return None
    calls = set()
    loops = 0
    returns = 0

    def expr(item):
        value = folded(item)
        if value is MISSING and isinstance(item, ast.Constant):
            value = item.value
        if type(value) is int:
            if not minimum <= value <= maximum:
                raise Ineligible
            return {"op": "constant", "value": value}
        if isinstance(item, ast.Name) and item.id in names:
            return {"op": "local", "name": item.id}
        if isinstance(item, ast.BinOp) and type(item.op) in (ast.Add, ast.Sub, ast.Mult):
            return {
                "op": {ast.Add: "add", ast.Sub: "subtract", ast.Mult: "multiply"}[type(item.op)],
                "left": expr(item.left),
                "right": expr(item.right),
            }
        if isinstance(item, ast.UnaryOp) and isinstance(item.op, (ast.UAdd, ast.USub)):
            return {
                "op": "positive" if isinstance(item.op, ast.UAdd) else "negative",
                "value": expr(item.operand),
            }
        if (
            isinstance(item, ast.Call)
            and isinstance(item.func, ast.Name)
            and item.func.id == node.name
            and node.name not in names
            and not node.args.kwonlyargs
            and not item.keywords
            and len(item.args) == len(parameters)
        ):
            calls.add(node.name)
            return {"op": "recurse", "args": [expr(arg) for arg in item.args]}
        raise Ineligible

    def condition(item):
        if (
            isinstance(item, ast.Compare)
            and len(item.ops) == 1
            and type(item.ops[0]) in (ast.Lt, ast.LtE, ast.Gt, ast.GtE, ast.Eq, ast.NotEq)
        ):
            return {
                "op": "compare",
                "comparison": type(item.ops[0]).__name__,
                "left": expr(item.left),
                "right": expr(item.comparators[0]),
            }
        return {"op": "truth", "value": expr(item)}

    def body(items):
        nonlocal loops, returns
        result = []
        for item in items:
            if (
                isinstance(item, ast.Expr)
                and isinstance(item.value, ast.Constant)
                and isinstance(item.value.value, str)
            ):
                continue
            if isinstance(item, ast.Assign) and all(
                isinstance(t, ast.Name) and t.id in names for t in item.targets
            ):
                result.append(
                    {
                        "op": "assign",
                        "targets": [t.id for t in item.targets],
                        "value": expr(item.value),
                    }
                )
            elif (
                isinstance(item, ast.AugAssign)
                and isinstance(item.target, ast.Name)
                and type(item.op) in (ast.Add, ast.Sub, ast.Mult)
            ):
                value = ast.BinOp(
                    left=ast.copy_location(ast.Name(item.target.id, ast.Load()), item.target),
                    op=item.op,
                    right=item.value,
                )
                ast.copy_location(value, item)
                result.append({"op": "assign", "targets": [item.target.id], "value": expr(value)})
            elif isinstance(item, ast.Return) and item.value is not None:
                returns += 1
                result.append({"op": "return", "value": expr(item.value)})
            elif isinstance(item, ast.If):
                result.append(
                    {
                        "op": "if",
                        "condition": condition(item.test),
                        "body": body(item.body),
                        "else": body(item.orelse),
                    }
                )
            elif (
                isinstance(item, ast.For)
                and isinstance(item.target, ast.Name)
                and isinstance(item.iter, ast.Call)
                and isinstance(item.iter.func, ast.Name)
                and item.iter.func.id == "range"
                and "range" not in names
                and not item.iter.keywords
                and 1 <= len(item.iter.args) <= 3
            ):
                loops += 1
                calls.add("range")
                result.append(
                    {
                        "op": "for",
                        "target": item.target.id,
                        "range": [expr(a) for a in item.iter.args],
                        "body": body(item.body),
                        "else": body(item.orelse),
                    }
                )
            elif isinstance(item, ast.While):
                loops += 1
                result.append(
                    {
                        "op": "while",
                        "condition": condition(item.test),
                        "body": body(item.body),
                        "else": body(item.orelse),
                    }
                )
            elif isinstance(item, (ast.Break, ast.Continue, ast.Pass)):
                result.append({"op": type(item).__name__.lower()})
            else:
                raise Ineligible
        return result

    try:
        statements = body(node.body)
    except (Ineligible, RecursionError):
        return None
    if not returns:
        return None
    return {
        "schema_version": 1,
        "kind": "checked-pure-integer",
        "bits": bits,
        "parameters": parameters,
        "locals": names,
        "guards": sorted(calls),
        "body": statements,
        "loops": loops,
        "enabled": False,
        "replay": "pure arithmetic and guarded builtin range only; no observable effects",
    }


def validate_kernel(fn):
    from ..models import CalciumInternalError

    spec = fn.kernel
    if spec is None:
        return

    def require(value):
        if not value:
            raise CalciumInternalError("invalid typed native kernel")

    require(spec.get("schema_version") == 1 and spec.get("kind") == "checked-pure-integer")
    require(spec.get("bits") == struct.calcsize("l") * 8)
    require(spec.get("parameters") == fn.parameters and spec.get("locals") == fn.locals)
    require(type(spec.get("enabled")) is bool and set(spec.get("guards", [])) <= {"range", fn.name})
    minimum, maximum = -(1 << (spec["bits"] - 1)), (1 << (spec["bits"] - 1)) - 1

    def expression(item):
        op = item.get("op")
        if op == "constant":
            require(type(item.get("value")) is int and minimum <= item["value"] <= maximum)
        elif op == "local":
            require(item.get("name") in fn.locals)
        elif op in {"add", "subtract", "multiply"}:
            expression(item["left"])
            expression(item["right"])
        elif op in {"positive", "negative"}:
            expression(item["value"])
        elif op == "recurse":
            require(
                fn.name in spec["guards"]
                and not fn.kwonly
                and len(item["args"]) == len(fn.parameters)
            )
            for arg in item["args"]:
                expression(arg)
        else:
            require(False)

    def condition(item):
        if item.get("op") == "truth":
            expression(item["value"])
        else:
            require(
                item.get("op") == "compare"
                and item.get("comparison") in {"Lt", "LtE", "Gt", "GtE", "Eq", "NotEq"}
            )
            expression(item["left"])
            expression(item["right"])

    def statements(items, depth=0):
        for item in items:
            op = item.get("op")
            if op == "assign":
                require(bool(item["targets"]) and set(item["targets"]) <= set(fn.locals))
                expression(item["value"])
            elif op == "return":
                expression(item["value"])
            elif op == "if":
                condition(item["condition"])
                statements(item["body"], depth)
                statements(item["else"], depth)
            elif op in {"for", "while"}:
                if op == "for":
                    require(
                        item["target"] in fn.locals
                        and "range" in spec["guards"]
                        and 1 <= len(item["range"]) <= 3
                    )
                    for value in item["range"]:
                        expression(value)
                else:
                    condition(item["condition"])
                statements(item["body"], depth + 1)
                statements(item["else"], depth)
            elif op in {"break", "continue"}:
                require(depth > 0)
            else:
                require(op == "pass")

    statements(spec["body"])
