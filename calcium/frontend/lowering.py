"""Target-version compiler decisions used without evaluating analyzed expressions."""

import ast
import dis


def incremental_dict(node):
    # CPython changes evaluation/hash ordering for large literals. Inspect an
    # equivalent shape with inert names so nested dicts cannot affect the answer.
    shape = ast.Dict(
        keys=[ast.Name(id="key", ctx=ast.Load()) for _ in node.keys],
        values=[ast.Name(id="value", ctx=ast.Load()) for _ in node.values],
    )
    expr = ast.fix_missing_locations(ast.Expression(shape))
    return any(
        i.opname == "MAP_ADD" for i in dis.get_instructions(compile(expr, "<dict-shape>", "eval"))
    )
