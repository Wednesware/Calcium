"""Conservative standalone capability gate and annotation normalization."""

import ast
import builtins
import copy
import warnings

from ...models import Diagnostic
from .builtins import SUPPORTED, UNSUPPORTED

ALLOWED = (
    ast.Module,
    ast.Match,
    ast.match_case,
    ast.MatchValue,
    ast.MatchSingleton,
    ast.MatchOr,
    ast.MatchAs,
    ast.AsyncFunctionDef,
    ast.Await,
    ast.AsyncWith,
    ast.BitOr,
    ast.BitAnd,
    ast.BitXor,
    ast.Invert,
    ast.LShift,
    ast.RShift,
    ast.ClassDef,
    ast.AnnAssign,
    ast.Lambda,
    ast.Nonlocal,
    ast.Try,
    ast.ExceptHandler,
    ast.Raise,
    ast.Assert,
    ast.With,
    ast.withitem,
    ast.ListComp,
    ast.SetComp,
    ast.DictComp,
    ast.GeneratorExp,
    ast.comprehension,
    ast.JoinedStr,
    ast.FormattedValue,
    ast.NamedExpr,
    ast.Div,
    ast.FloorDiv,
    ast.Mod,
    ast.Pow,
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
    future_annotations = any(
        isinstance(n, ast.ImportFrom)
        and n.module == "__future__"
        and any(a.name == "annotations" for a in n.names)
        for n in tree.body
    )
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
        class_depth = 0
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
                and node.id not in SUPPORTED
                and node.id not in {"__name__", "__file__", "__package__"}
                and node.id not in self.locals_
            ):
                reason = UNSUPPORTED.get(node.id, "Native implementation unavailable")
                self.reject(node, f"builtin {node.id!r} is not implemented: {reason}")

        def visit_comprehension(self, node):
            if node.is_async:
                self.reject(node, "asynchronous comprehensions are not implemented")
            else:
                self.generic_visit(node)

        def visit_AnnAssign(self, node):
            self.visit(node.target)
            if node.value is not None:
                self.visit(node.value)
            if not future_annotations and any(
                isinstance(n, (ast.Call, ast.NamedExpr, ast.Lambda))
                for n in ast.walk(node.annotation)
            ):
                self.reject(node, "effectful annotation evaluation is not implemented")

        def visit_MatchAs(self, node):
            if node.name is not None:
                self.reject(node, "capturing match patterns are not implemented")
            else:
                self.generic_visit(node)

        def visit_ClassDef(self, node):
            if len(node.bases) > 1 or node.keywords or node.type_params:
                self.reject(
                    node,
                    "multiple inheritance, metaclasses and generic classes are not implemented",
                )
                return
            for value in [*node.bases, *node.decorator_list]:
                self.visit(value)
            self.control += 1
            self.class_depth += 1
            for statement in node.body:
                self.visit(statement)
            self.control -= 1
            self.class_depth -= 1

        def visit_Lambda(self, node):
            for default in [
                *node.args.defaults,
                *(v for v in node.args.kw_defaults if v is not None),
            ]:
                self.visit(default)
            self.visit(node.body)

        def visit_Compare(self, node):
            self.generic_visit(node)

        def visit_List(self, node):
            self.generic_visit(node)

        visit_Tuple = visit_List
        visit_Set = visit_List
        visit_Dict = visit_List

        def visit_ImportFrom(self, node):
            if node.module == "__future__":
                if any(a.name != "annotations" for a in node.names):
                    self.reject(node, "only the annotations future feature is implemented")
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
            if type(node.value) not in (type(None), bool, int, float, str, bytes):
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
            if node.name in {
                "__getattribute__",
                "__setattr__",
                "__delattr__",
                "__new__",
                "__init_subclass__",
                "__set_name__",
            }:
                self.reject(node, "custom object construction/attribute hooks are not implemented")
                return
            if node.name == "__getattr__" and not self.class_depth and not self.depth:
                self.reject(node, "dynamic module __getattr__ is not implemented")
                return
            if node.type_params:
                self.reject(node, "generic type parameter definitions are not yet supported")
                return
            for decorator in node.decorator_list:
                self.visit(decorator)
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
                    and any(
                        isinstance(n, (ast.Call, ast.Lambda, ast.NamedExpr))
                        for n in ast.walk(annotation)
                    )
                    and not future_annotations
                ):
                    self.reject(annotation, "effectful annotation evaluation is not implemented")
            for default in [*args.defaults, *(d for d in args.kw_defaults if d is not None)]:
                self.visit(default)
            previous = self.locals_
            self.locals_ = {a.arg for a in annotated_parameters}
            self.depth += 1
            for statement in node.body:
                self.visit(statement)
            self.depth -= 1
            self.locals_ = previous

        visit_AsyncFunctionDef = visit_FunctionDef

        def visit_If(self, node):
            self.control += 1
            self.generic_visit(node)
            self.control -= 1

        visit_While = visit_If
        visit_For = visit_If

    Check().visit(tree)
    normalized = copy.deepcopy(tree)
    normalized.body = [
        n
        for n in normalized.body
        if not (isinstance(n, ast.ImportFrom) and n.module == "__future__")
    ]
    for node in ast.walk(normalized):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            node.returns = None
            for arg in (
                node.args.posonlyargs
                + node.args.args
                + node.args.kwonlyargs
                + [a for a in (node.args.vararg, node.args.kwarg) if a]
            ):
                arg.annotation = None
    return tree, normalized, errors
