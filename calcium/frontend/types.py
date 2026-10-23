"""Conservative flow facts for inspection; these facts never authorize native types."""

import ast

UNKNOWN = frozenset({"Unknown"})
UNBOUND = frozenset({"Unbound"})


def union(a, b):
    return UNKNOWN if "Unknown" in a | b else a | b


def join(a, b):
    return {name: union(a.get(name, UNBOUND), b.get(name, UNBOUND)) for name in a.keys() | b.keys()}


def infer(tree):
    reports = {}

    def expression(node, facts):
        if isinstance(node, ast.Constant):
            return frozenset({type(node.value).__name__})
        if isinstance(node, ast.Name):
            return facts.get(node.id, UNKNOWN)
        if isinstance(node, (ast.List, ast.Tuple, ast.Dict, ast.Set)):
            return frozenset({type(node).__name__.lower()})
        if isinstance(node, ast.UnaryOp):
            if isinstance(node.op, ast.Not):
                return frozenset({"bool"})
            types = expression(node.operand, facts)
            return (
                frozenset({"int"})
                if types <= {"int", "bool"}
                else types
                if types == {"float"}
                else UNKNOWN
            )
        if isinstance(node, ast.BinOp):
            left, right = expression(node.left, facts), expression(node.right, facts)
            if left <= {"int", "bool"} and right <= {"int", "bool"}:
                # Negative integer powers may produce floats.
                return (
                    frozenset({"float"})
                    if isinstance(node.op, ast.Div)
                    else frozenset({"int", "float"})
                    if isinstance(node.op, ast.Pow)
                    else frozenset({"int"})
                )
            if left <= {"float", "int", "bool"} and right <= {"float", "int", "bool"}:
                return (
                    frozenset({"float", "complex"})
                    if isinstance(node.op, ast.Pow)
                    else frozenset({"float"})
                )
            if (
                left == right
                and left
                in (
                    frozenset({"str"}),
                    frozenset({"bytes"}),
                    frozenset({"list"}),
                    frozenset({"tuple"}),
                )
                and isinstance(node.op, ast.Add)
            ):
                return left
        if isinstance(node, ast.BoolOp):
            result = frozenset()
            for value in node.values:
                result = union(result, expression(value, facts))
            return result
        if isinstance(node, ast.IfExp):
            return union(expression(node.body, facts), expression(node.orelse, facts))
        # Overloaded comparisons can return arbitrary objects; calls and indexing
        # can execute user code. Do not infer builtin return types from their names.
        return UNKNOWN

    def record(scope, node, name, types):
        key = (scope, node.lineno, name)
        old = reports.get(key)
        merged = union(frozenset(old["types"]), types) if old else types
        reports[key] = {
            "scope": scope,
            "line": node.lineno,
            "name": name,
            "types": sorted(merged),
            "evidence": "conservative flow analysis",
            "representation": "PyObject*",
        }

    def walk(nodes, facts, scope):
        facts = facts.copy()
        for node in nodes:
            if (
                scope == "<module>"
                and not isinstance(node, ast.FunctionDef)
                and any(isinstance(item, ast.Call) for item in ast.walk(node))
            ):
                facts = {name: UNKNOWN for name in facts}
            if isinstance(node, (ast.Break, ast.Continue, ast.Return, ast.Raise)):
                break
            if isinstance(node, (ast.Assign, ast.AnnAssign)):
                value = expression(node.value, facts) if node.value else UNKNOWN
                targets = node.targets if isinstance(node, ast.Assign) else [node.target]
                for target in targets:
                    if isinstance(target, ast.Name):
                        facts[target.id] = value
                        record(scope, node, target.id, value)
            elif isinstance(node, ast.FunctionDef):
                facts[node.name] = frozenset({"callable"})
                local = {
                    n.id: UNBOUND
                    for n in ast.walk(node)
                    if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Store)
                }
                local.update(
                    {
                        a.arg: UNKNOWN
                        for a in node.args.args + node.args.posonlyargs + node.args.kwonlyargs
                    }
                )
                walk(node.body, local, node.name)
            elif isinstance(node, ast.If):
                facts = join(walk(node.body, facts, scope), walk(node.orelse, facts, scope))
            elif isinstance(node, (ast.While, ast.For)):
                entry = facts.copy()
                if isinstance(node, ast.For) and isinstance(node.target, ast.Name):
                    facts[node.target.id] = UNKNOWN
                for _ in range(16):
                    updated = join(entry, walk(node.body, facts, scope))
                    if updated == facts:
                        break
                    facts = updated
                else:
                    facts = {name: UNKNOWN for name in facts}
                facts = join(facts, walk(node.orelse, facts, scope))
            elif isinstance(node, ast.AugAssign) and isinstance(node.target, ast.Name):
                facts[node.target.id] = UNKNOWN
            elif any(isinstance(item, ast.Call) for item in ast.walk(node)):
                # Unknown calls may mutate module bindings. Function local names
                # cannot be rebound through ordinary dynamic dispatch.
                if scope == "<module>":
                    facts = {name: UNKNOWN for name in facts}
        return facts

    walk(tree.body, {}, "<module>")
    return sorted(reports.values(), key=lambda row: (row["scope"], row["line"], row["name"]))
