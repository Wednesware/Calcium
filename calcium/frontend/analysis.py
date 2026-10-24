"""Parse, validate scope syntax and conservatively classify native capability."""

import ast
import symtable
import warnings
from dataclasses import dataclass

from ..models import Diagnostic
from .loader import Source
from .types import infer

BINARY = {
    ast.Add: "add",
    ast.Sub: "subtract",
    ast.Mult: "multiply",
    ast.Div: "true_divide",
    ast.FloorDiv: "floor_divide",
    ast.Mod: "remainder",
    ast.Pow: "power",
    ast.LShift: "lshift",
    ast.RShift: "rshift",
    ast.BitAnd: "and",
    ast.BitOr: "or",
    ast.BitXor: "xor",
}
UNARY = {ast.UAdd: "positive", ast.USub: "negative", ast.Invert: "invert", ast.Not: "not"}
COMPARE = {
    ast.Eq: "eq",
    ast.NotEq: "ne",
    ast.Lt: "lt",
    ast.LtE: "le",
    ast.Gt: "gt",
    ast.GtE: "ge",
    ast.Is: "is",
    ast.IsNot: "is_not",
    ast.In: "in",
    ast.NotIn: "not_in",
}
REFLECTION = {
    "eval",
    "exec",
    "locals",
    "globals",
    "vars",
    "dir",
    "breakpoint",
    "super",
    "__builtins__",
}


@dataclass
class Analysis:
    tree: ast.Module
    diagnostics: list[Diagnostic]
    fallbacks: list[dict]
    scopes: list[dict]
    types: list[dict]
    definition_fallbacks: set[int]


class Capability(ast.NodeVisitor):
    def __init__(self, source: Source):
        self.source = source
        self.reasons: list[dict] = []
        self.depth = 0
        self.definition_fallbacks: set[int] = set()

    def reject(self, node: ast.AST, reason: str):
        line = getattr(node, "lineno", 1)
        offset = getattr(node, "col_offset", 0)
        text = self.source.text.splitlines()[line - 1] if self.source.text.splitlines() else ""
        column = len(text.encode("utf-8")[:offset].decode("utf-8")) + 1
        self.reasons.append(
            {
                "line": line,
                "column": column,
                "construct": type(node).__name__,
                "reason": reason,
                "boundary": "module",
            }
        )

    def generic_visit(self, node):
        self.reject(node, f"{type(node).__name__} has no native lowering yet")

    def visit_Module(self, node):
        for item in node.body:
            self.visit(item)

    def visit_Constant(self, node):
        if not isinstance(node.value, (type(None), bool, int, float, str, bytes)):
            self.reject(node, "constant type requires CPython code-object execution")

    def visit_Name(self, node):
        if node.id in REFLECTION:
            self.reject(node, "frame-sensitive builtin or mutable builtins mapping")

    def visit_Import(self, node):
        pass

    def visit_ImportFrom(self, node):
        if node.module == "__future__":
            self.reject(node, "future compiler flags require whole-module fallback")
        if any(alias.name == "*" for alias in node.names):
            self.reject(node, "star imports require code-object fallback")

    def visit_Expr(self, node):
        self.visit(node.value)

    def visit_Assign(self, node):
        self.visit(node.value)
        for target in node.targets:
            if not isinstance(
                target, (ast.Name, ast.Attribute, ast.Subscript, ast.Tuple, ast.List)
            ):
                self.reject(target, "unpacking assignment requires fallback")
            else:
                self.visit(target)

    def visit_AugAssign(self, node):
        if (
            not isinstance(node.target, (ast.Name, ast.Attribute, ast.Subscript))
            or type(node.op) not in BINARY
        ):
            self.reject(node, "augmented assignment operator requires fallback")
        else:
            self.visit(node.target)
        self.visit(node.value)

    def visit_Attribute(self, node):
        if node.attr in {
            "__code__",
            "__globals__",
            "__closure__",
            "__defaults__",
            "__annotations__",
            "__dict__",
        }:
            self.reject(node, "native function metadata is not Python function metadata")
        self.visit(node.value)

    def visit_Subscript(self, node):
        self.visit(node.value)
        self.visit(node.slice)

    def visit_Slice(self, node):
        for item in (node.lower, node.upper, node.step):
            if item is not None:
                self.visit(item)

    def visit_BinOp(self, node):
        if type(node.op) not in BINARY:
            self.reject(node, "operator requires fallback")
        self.visit(node.left)
        self.visit(node.right)

    def visit_UnaryOp(self, node):
        if type(node.op) not in UNARY:
            self.reject(node, "unary operator requires fallback")
        self.visit(node.operand)

    def visit_BoolOp(self, node):
        for value in node.values:
            self.visit(value)

    def visit_Compare(self, node):
        self.visit(node.left)
        for value in node.comparators:
            self.visit(value)

    def visit_IfExp(self, node):
        for value in (node.test, node.body, node.orelse):
            self.visit(value)

    def visit_Call(self, node):
        if isinstance(node.func, ast.Name) and node.func.id == "type" and len(node.args) > 1:
            self.reject(
                node, "dynamic class construction requires Python function descriptor behavior"
            )
        self.visit(node.func)
        for arg in node.args:
            if isinstance(arg, ast.Starred):
                self.visit(arg.value)
            else:
                self.visit(arg)
        for keyword in node.keywords:
            self.visit(keyword.value)

    def visit_List(self, node):
        for item in node.elts:
            self.visit(item)

    visit_Tuple = visit_List

    def visit_Set(self, node):
        for item in node.elts:
            if isinstance(item, ast.Starred):
                self.reject(item, "set expansion requires fallback")
            else:
                self.visit(item)

    def visit_Starred(self, node):
        self.visit(node.value)

    def visit_Dict(self, node):
        if any(key is not None and not isinstance(key, ast.Constant) for key in node.keys):
            self.reject(
                node,
                "dynamic dictionary hashing retains CPython construction order through fallback",
            )
        for key, value in zip(node.keys, node.values):
            if key is None:
                pass
            else:
                self.visit(key)
            self.visit(value)

    def visit_If(self, node):
        self.visit(node.test)
        for item in node.body + node.orelse:
            self.visit(item)

    visit_While = visit_If

    def visit_For(self, node):
        self.visit(node.target)
        self.visit(node.iter)
        for item in node.body + node.orelse:
            self.visit(item)

    def visit_FunctionDef(self, node):
        start = len(self.reasons)
        args = node.args
        if "\x00" in (ast.get_docstring(node, clean=False) or ""):
            self.reject(node, "function docstrings containing NUL require fallback")
        if self.depth or node.decorator_list or node.returns or node.type_params:
            self.reject(node, "nested/decorated/annotated/generic functions require fallback")
        all_args = args.posonlyargs + args.args + args.kwonlyargs
        all_args += [arg for arg in (args.vararg, args.kwarg) if arg is not None]
        if any(arg.annotation is not None for arg in all_args):
            self.reject(node, "annotations retain runtime behavior through fallback")
        for value in args.defaults + [v for v in args.kw_defaults if v is not None]:
            self.visit(value)
        self.depth += 1
        for item in node.body:
            self.visit(item)
        self.depth -= 1
        if len(self.reasons) > start:
            self.definition_fallbacks.add(id(node))
            for reason in self.reasons[start:]:
                reason.update(boundary="definition", definition=node.name)

    def visit_ClassDef(self, node):
        self.reject(node, "class bodies execute through reported Python definition fallback")
        self.definition_fallbacks.add(id(node))
        self.reasons[-1].update(boundary="definition", definition=node.name)

    def visit_AsyncFunctionDef(self, node):
        self.reject(node, "async functions execute through reported Python definition fallback")
        self.definition_fallbacks.add(id(node))
        self.reasons[-1].update(boundary="definition", definition=node.name)

    def visit_Return(self, node):
        if node.value is not None:
            self.visit(node.value)

    def visit_NamedExpr(self, node):
        self.visit(node.target)
        self.visit(node.value)

    def visit_JoinedStr(self, node):
        for item in node.values:
            self.visit(item)

    def visit_FormattedValue(self, node):
        self.visit(node.value)
        if node.format_spec is not None:
            self.visit(node.format_spec)

    def visit_Delete(self, node):
        for target in node.targets:
            self.visit(target)

    def visit_Assert(self, node):
        self.visit(node.test)
        if node.msg is not None:
            self.visit(node.msg)

    def visit_Raise(self, node):
        if node.exc is not None:
            self.visit(node.exc)
        if node.cause is not None:
            self.visit(node.cause)

    def visit_Global(self, node):
        pass

    def visit_Pass(self, node):
        pass

    visit_Break = visit_Pass
    visit_Continue = visit_Pass


def analyze(source: Source) -> Analysis:
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", SyntaxWarning)
        tree = ast.parse(source.text, filename=str(source.path))
        # Compile for semantic validation only; do not execute the resulting code.
        compile(tree, str(source.path), "exec", dont_inherit=True, optimize=0)
    diagnostics = [
        Diagnostic("C105", str(w.message), str(source.path), w.lineno, severity="warning")
        for w in caught
    ]
    symbols = symtable.symtable(source.text, str(source.path), "exec")
    scopes = []

    def describe(table):
        scopes.append(
            {
                "name": table.get_name(),
                "kind": str(table.get_type()),
                "locals": sorted(s.get_name() for s in table.get_symbols() if s.is_local()),
                "globals": sorted(s.get_name() for s in table.get_symbols() if s.is_global()),
                "free": sorted(s.get_name() for s in table.get_symbols() if s.is_free()),
            }
        )
        for child in table.get_children():
            describe(child)

    describe(symbols)
    capability = Capability(source)
    capability.visit(tree)
    types = [
        {
            "line": node.lineno,
            "type": type(node.value).__name__,
            "evidence": "literal",
            "representation": "PyObject*",
        }
        for node in ast.walk(tree)
        if isinstance(node, ast.Constant)
    ]
    return Analysis(
        tree,
        diagnostics,
        capability.reasons,
        scopes,
        types + infer(tree),
        capability.definition_fallbacks,
    )
