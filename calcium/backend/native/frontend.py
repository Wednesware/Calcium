"""Conservative standalone capability gate and annotation normalization."""

import ast
import builtins
import copy
import symtable
import warnings

from ...models import Diagnostic

ALLOWED = (
    ast.Module,
    ast.Import,
    ast.ImportFrom,
    ast.alias,
    ast.Attribute,
    ast.List,
    ast.Tuple,
    ast.Dict,
    ast.Set,
    ast.Subscript,
    ast.Slice,
    ast.Starred,
    ast.In,
    ast.NotIn,
    ast.Is,
    ast.IsNot,
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

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            globals_.update(a.asname or a.name.split(".")[0] for a in node.names)
        elif isinstance(node, ast.ImportFrom):
            globals_.update(a.asname or a.name for a in node.names)

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
                    f"Bonemarrow: {reason}; use --cpython for compatibility (no automatic fallback)",
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
                    or node.id in {"__spec__", "__cached__", "__annotations__", "__builtins__"}
                )
                and node.id
                not in {
                    "print",
                    "range",
                    "__name__",
                    "__file__",
                    "__package__",
                    "len",
                    "list",
                    "tuple",
                    "dict",
                    "set",
                }
                and node.id not in self.locals_
            ):
                self.reject(node, f"builtin {node.id!r} is not implemented")

        def visit_Compare(self, node):
            operands = [node.left, *node.comparators]
            for i, op in enumerate(node.ops):
                if isinstance(op, (ast.Is, ast.IsNot)) and not any(
                    isinstance(v, ast.Constant) and (v.value is None or type(v.value) is bool)
                    for v in operands[i : i + 2]
                ):
                    self.reject(
                        node,
                        "identity comparisons require None or bool; general object identity is not implemented",
                    )
                    return
            self.generic_visit(node)

        def visit_List(self, node):
            if len(node.elts) > 128:
                self.reject(
                    node, "container literals with more than 128 elements are not supported"
                )
            else:
                self.generic_visit(node)

        visit_Tuple = visit_List
        visit_Set = visit_List

        def visit_Dict(self, node):
            if len(node.keys) > 64:
                self.reject(node, "dictionary literals with more than 64 entries are not supported")
            else:
                self.generic_visit(node)

        def visit_ImportFrom(self, node):
            if node.module == "__future__":
                self.reject(node, "future flags are not supported")
                return
            if any(a.name == "*" for a in node.names):
                self.reject(node, "star imports are not supported")
            else:
                self.generic_visit(node)

        def visit_Constant(self, node):
            if type(node.value) is int and node.value.bit_length() > 14000:
                try:
                    str(node.value)
                except ValueError:
                    self.reject(
                        node, "integer literal exceeds the frontend decimal conversion limit"
                    )
            if type(node.value) not in (type(None), bool, int, float, str):
                self.reject(node, f"{type(node.value).__name__} values are not implemented")
            if isinstance(node.value, str) and any(0xD800 <= ord(c) <= 0xDFFF for c in node.value):
                self.reject(node, "surrogate string code points are not supported")

        def visit_Assign(self, node):
            if any(
                not isinstance(t, (ast.Name, ast.Attribute, ast.Subscript, ast.Tuple, ast.List))
                for t in node.targets
            ):
                self.reject(node, "only name assignment is supported")
            else:
                self.generic_visit(node)

        def visit_Subscript(self, node):
            if isinstance(node.ctx, ast.Store) and isinstance(node.slice, ast.Slice):
                self.reject(node, "slice assignment is not implemented")
            else:
                self.generic_visit(node)

        def visit_Starred(self, node):
            if isinstance(node.ctx, ast.Store):
                self.reject(node, "starred unpacking assignment is not implemented")
            else:
                self.generic_visit(node)

        def visit_AugAssign(self, node):
            if not isinstance(node.target, (ast.Name, ast.Attribute, ast.Subscript)):
                self.reject(node, "only name augmented assignment is supported")
            else:
                self.generic_visit(node)

        def visit_Call(self, node):
            if len(node.args) + len(node.keywords) > 128:
                self.reject(node, "calls with more than 128 arguments are not supported")
                return
            self.generic_visit(node)

        def visit_FunctionDef(self, node):
            if node.name == "__getattr__":
                self.reject(node, "dynamic module __getattr__ is not implemented")
                return
            if self.depth or self.control or node.decorator_list or node.type_params:
                self.reject(
                    node, "only undecorated module-level non-generic functions are supported"
                )
                return
            args = node.args
            parameters = args.posonlyargs + args.args + args.kwonlyargs
            annotated_parameters = parameters + [a for a in (args.vararg, args.kwarg) if a]
            if len(parameters) + bool(args.vararg) + bool(args.kwarg) > 128:
                self.reject(
                    node,
                    "more than 128 parameters are not supported",
                )
                return
            for annotation in [node.returns, *(a.annotation for a in annotated_parameters)]:
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
            for default in [*args.defaults, *(d for d in args.kw_defaults if d is not None)]:
                self.visit(default)
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
            for arg in (
                node.args.posonlyargs
                + node.args.args
                + node.args.kwonlyargs
                + [a for a in (node.args.vararg, node.args.kwarg) if a]
            ):
                arg.annotation = None
    return tree, normalized, errors
