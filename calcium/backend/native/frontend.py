"""Conservative standalone capability gate and annotation normalization."""

import ast
import builtins
import copy
import symtable
import warnings

from ...models import Diagnostic

ALLOWED = (
    ast.Module,
    ast.Expr,
    ast.Pass,
    ast.Assign,
    ast.AugAssign,
    ast.Return,
    ast.If,
    ast.While,
    ast.For,
    ast.Break,
    ast.Continue,
    ast.Global,
    ast.Name,
    ast.Constant,
    ast.BinOp,
    ast.UnaryOp,
    ast.BoolOp,
    ast.Compare,
    ast.IfExp,
    ast.Call,
    ast.keyword,
    ast.Load,
    ast.Store,
    ast.Add,
    ast.Sub,
    ast.Mult,
    ast.UAdd,
    ast.USub,
    ast.Not,
    ast.And,
    ast.Or,
    ast.Eq,
    ast.NotEq,
    ast.Lt,
    ast.LtE,
    ast.Gt,
    ast.GtE,
)


def prepare(source):
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", SyntaxWarning)
        tree = ast.parse(source.text, filename=str(source.path))
        compile(tree, str(source.path), "exec", dont_inherit=True, optimize=0)
        symbols = symtable.symtable(source.text, str(source.path), "exec")
    scopes = {(s.get_name(), s.get_lineno()): s for s in symbols.get_children()}
    errors = [
        Diagnostic("C105", message, str(source.path), line, severity="warning")
        for line, message in sorted({(w.lineno, str(w.message)) for w in caught})
    ]
    globals_ = {
        n.id for n in ast.walk(tree) if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Store)
    }
    globals_.update(n.name for n in tree.body if isinstance(n, ast.FunctionDef))

    class Check(ast.NodeVisitor):
        depth = 0
        control = 0
        locals_ = set()

        def reject(self, node, reason):
            line = getattr(node, "lineno", 1)
            offset = getattr(node, "col_offset", 0)
            column = (
                len(source.text.splitlines()[line - 1].encode("utf-8")[:offset].decode("utf-8")) + 1
            )
            errors.append(
                Diagnostic(
                    "C160",
                    f"native backend: {reason}; no CPython fallback is available",
                    str(source.path),
                    line,
                    column,
                )
            )

        def generic_visit(self, node):
            if not isinstance(node, ALLOWED):
                self.reject(
                    node, f"{type(node).__name__} is not supported in the standalone milestone"
                )
                return
            super().generic_visit(node)

        def visit_Name(self, node):
            if (
                isinstance(node.ctx, ast.Load)
                and (
                    hasattr(builtins, node.id)
                    or node.id
                    in {"__file__", "__package__", "__spec__", "__cached__", "__annotations__"}
                )
                and node.id not in {"print", "range"}
                and node.id not in self.locals_
            ):
                self.reject(node, f"builtin {node.id!r} is not implemented")

        def visit_Constant(self, node):
            if type(node.value) not in (type(None), bool, int, float, str):
                self.reject(node, f"{type(node.value).__name__} values are not implemented")
            if isinstance(node.value, str) and any(0xD800 <= ord(c) <= 0xDFFF for c in node.value):
                self.reject(node, "surrogate string code points are not supported")

        def visit_Assign(self, node):
            if any(not isinstance(t, ast.Name) for t in node.targets):
                self.reject(node, "only name assignment is supported")
            else:
                self.generic_visit(node)

        def visit_AugAssign(self, node):
            if not isinstance(node.target, ast.Name):
                self.reject(node, "only name augmented assignment is supported")
            else:
                self.generic_visit(node)

        def visit_Call(self, node):
            if len(node.args) + len(node.keywords) > 128:
                self.reject(node, "calls with more than 128 arguments are not supported")
                return
            if any(isinstance(a, ast.Starred) for a in node.args) or any(
                k.arg is None for k in node.keywords
            ):
                self.reject(node, "expanded calls are not supported")
            else:
                self.generic_visit(node)

        def visit_FunctionDef(self, node):
            if self.depth or self.control or node.decorator_list or node.type_params:
                self.reject(
                    node, "only undecorated module-level non-generic functions are supported"
                )
                return
            args = node.args
            parameters = args.posonlyargs + args.args
            if (
                args.defaults
                or args.kwonlyargs
                or args.vararg
                or args.kwarg
                or len(parameters) > 128
            ):
                self.reject(
                    node,
                    "defaults, keyword-only/variadic parameters or more than 128 parameters are not supported",
                )
                return
            for annotation in [node.returns, *(a.annotation for a in parameters)]:
                if (
                    annotation is not None
                    and not (isinstance(annotation, ast.Constant) and annotation.value is None)
                    and not (
                        isinstance(annotation, ast.Name)
                        and annotation.id in {"int", "float", "bool", "str"}
                        and annotation.id not in globals_
                    )
                ):
                    self.reject(
                        annotation, "only unshadowed builtin primitive annotations are supported"
                    )
            previous = self.locals_
            scope = scopes[(node.name, node.lineno)]
            self.locals_ = {s.get_name() for s in scope.get_symbols() if s.is_local()}
            self.depth += 1
            for statement in node.body:
                self.visit(statement)
            self.depth -= 1
            self.locals_ = previous

        def visit_If(self, node):
            self.control += 1
            self.generic_visit(node)
            self.control -= 1

        visit_While = visit_If
        visit_For = visit_If

    Check().visit(tree)
    normalized = copy.deepcopy(tree)
    for node in ast.walk(normalized):
        if isinstance(node, ast.FunctionDef):
            node.returns = None
            for arg in node.args.posonlyargs + node.args.args:
                arg.annotation = None
    return tree, normalized, errors
