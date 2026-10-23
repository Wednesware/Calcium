import ast
import sys
from types import CodeType

from ..frontend.analysis import BINARY, COMPARE, UNARY
from ..frontend.constants import MISSING, encode, folded
from .nodes import Block, Function, Instruction, Program, Terminator


class Builder:
    def __init__(self, node: ast.Module | ast.FunctionDef, symbol: str, functions: dict[int, str]):
        module = isinstance(node, ast.Module)
        params = [] if module else [a.arg for a in node.args.args]
        # Nested functions are classified as fallback before CIR construction.
        locals_ = []
        if not module:
            # CPython's scope allocator supplies the target-version local order;
            # compiling creates a code object but never executes the function.
            scope = compile(
                ast.Module(body=[node], type_ignores=[]), "<scope>", "exec", dont_inherit=True
            )
            code = next(value for value in scope.co_consts if isinstance(value, CodeType))
            locals_ = list(code.co_varnames)
        doc = ast.get_docstring(node, clean=False)
        self.fn = Function(
            "<module>" if module else node.name,
            symbol,
            params,
            locals_,
            doc=doc,
            module=module,
        )
        self.functions = functions
        self.current = self.block()
        self.loops: list[tuple[str, str]] = []
        body = node.body
        if (
            body
            and isinstance(body[0], ast.Expr)
            and isinstance(body[0].value, ast.Constant)
            and isinstance(body[0].value.value, str)
        ):
            if module:
                value = self.expr(body[0].value)
                self.emit("store", [value], data="__doc__", result=False)
                self.drop(value)
            body = body[1:]
        self.statements(body)
        if self.current.terminator is None:
            value = self.constant(None)
            self.current.terminator = Terminator("return", [value])
        # CPython 3.14 changed frame local teardown from declaration order to
        # reverse order. Preserve that observable order in the lifetime IR.
        hidden = [name for name in reversed(self.fn.locals) if name.startswith("$")]
        declared = [name for name in self.fn.locals if not name.startswith("$")]
        self.fn.cleanup_order = hidden + (
            list(reversed(declared)) if sys.version_info >= (3, 14) else declared
        )

    def block(self):
        block = Block(f"b{len(self.fn.blocks)}")
        self.fn.blocks.append(block)
        return block

    def reg(self):
        value = self.fn.registers
        self.fn.registers += 1
        return value

    def emit(self, op, args=None, *, data=None, line=1, result=True, raises=True):
        value = self.reg() if result else None
        self.current.instructions.append(
            Instruction(op, args or [], value, data, line, "error" if raises else None)
        )
        return value

    def drop(self, value):
        self.emit("drop", [value], result=False, raises=False)

    def constant(self, value, line=1):
        # repr is data, never C text; the backend chooses construction APIs.
        return self.emit("constant", data=encode(value), line=line)

    def jump(self, block):
        if self.current.terminator is None:
            self.current.terminator = Terminator("jump", targets=[block.name])

    def hidden(self):
        name = f"$tmp{len(self.fn.locals)}"
        self.fn.locals.append(name)
        return name

    def save(self, name, value, line=1):
        self.emit("store", [value], data=name, result=False, line=line)

    def load(self, name, line=1):
        return self.emit("load", data=name, line=line)

    def condition(self, node, yes, no):
        """A truth context must not re-test a short-circuited user object."""
        if isinstance(node, ast.BoolOp):
            for value in node.values[:-1]:
                onward = self.block()
                self.condition(
                    value,
                    onward if isinstance(node.op, ast.And) else yes,
                    no if isinstance(node.op, ast.And) else onward,
                )
                self.current = onward
            self.condition(node.values[-1], yes, no)
        elif isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            self.condition(node.operand, no, yes)
        elif isinstance(node, ast.IfExp):
            a, b = self.block(), self.block()
            self.condition(node.test, a, b)
            self.current = a
            self.condition(node.body, yes, no)
            self.current = b
            self.condition(node.orelse, yes, no)
        else:
            value = self.expr(node)
            self.current.terminator = Terminator(
                "branch", [value], [yes.name, no.name], error="error"
            )

    def branch_value(self, node, slot, yes, no):
        # 3.12/3.13 materialize a nested BoolOp result before testing it again;
        # 3.14 fuses its jumps. __bool__ callbacks can observe the difference.
        if isinstance(node, ast.BoolOp) and sys.version_info >= (3, 14):
            for value in node.values[:-1]:
                onward = self.block()
                self.branch_value(
                    value,
                    slot,
                    onward if isinstance(node.op, ast.And) else yes,
                    no if isinstance(node.op, ast.And) else onward,
                )
                self.current = onward
                self.emit("clear", data=slot, result=False, raises=False)
            self.branch_value(node.values[-1], slot, yes, no)
        else:
            value = self.expr(node)
            self.save(slot, value)
            self.current.terminator = Terminator(
                "branch", [value], [yes.name, no.name], error="error"
            )

    def value_to(self, node, slot, end):
        if isinstance(node, ast.BoolOp):
            for value in node.values[:-1]:
                onward = self.block()
                self.branch_value(
                    value,
                    slot,
                    onward if isinstance(node.op, ast.And) else end,
                    end if isinstance(node.op, ast.And) else onward,
                )
                self.current = onward
                self.emit("clear", data=slot, result=False, raises=False)
            self.value_to(node.values[-1], slot, end)
        else:
            value = self.expr(node)
            self.save(slot, value)
            self.drop(value)
            self.jump(end)

    def expr(self, node):
        line = node.lineno
        value = folded(node)
        if value is not MISSING:
            return self.constant(value, line)
        if isinstance(node, ast.Constant):
            return self.constant(node.value, line)
        if isinstance(node, ast.Name):
            return self.load(node.id, line)
        if isinstance(node, ast.BinOp):
            a, b = self.expr(node.left), self.expr(node.right)
            out = self.emit("binary", [a, b], data=BINARY[type(node.op)], line=line)
            self.drop(a)
            self.drop(b)
            return out
        if isinstance(node, ast.UnaryOp):
            value = self.expr(node.operand)
            out = self.emit("unary", [value], data=UNARY[type(node.op)], line=line)
            self.drop(value)
            return out
        if isinstance(node, ast.Compare):
            a, b = self.expr(node.left), self.expr(node.comparators[0])
            out = self.emit("compare", [a, b], data=COMPARE[type(node.ops[0])], line=line)
            self.drop(a)
            self.drop(b)
            return out
        if isinstance(node, ast.Attribute):
            value = self.expr(node.value)
            out = self.emit("attribute", [value], data=node.attr, line=line)
            self.drop(value)
            return out
        if isinstance(node, ast.Subscript):
            a, b = self.expr(node.value), self.expr(node.slice)
            out = self.emit("getitem", [a, b], line=line)
            self.drop(a)
            self.drop(b)
            return out
        if isinstance(node, ast.Slice):
            args = [
                self.expr(n) if n is not None else self.constant(None)
                for n in (node.lower, node.upper, node.step)
            ]
            out = self.emit("slice", args, line=line)
            for arg in args:
                self.drop(arg)
            return out
        if isinstance(node, ast.Call):
            args = (
                [self.expr(node.func)]
                + [self.expr(a) for a in node.args]
                + [self.expr(k.value) for k in node.keywords]
            )
            out = self.emit(
                "call",
                args,
                data={"positional": len(node.args), "keywords": [k.arg for k in node.keywords]},
                line=line,
            )
            for arg in args:
                self.drop(arg)
            return out
        if isinstance(node, (ast.List, ast.Tuple, ast.Set, ast.Dict)):
            items = (
                [v for pair in zip(node.keys, node.values) for v in pair]
                if isinstance(node, ast.Dict)
                else node.elts
            )
            args = [self.expr(a) for a in items]
            out = self.emit("container", args, data=type(node).__name__.lower(), line=line)
            for arg in args:
                self.drop(arg)
            return out
        if isinstance(node, ast.IfExp):
            yes, no, join = self.block(), self.block(), self.block()
            name = self.hidden()
            self.condition(node.test, yes, no)
            for block, expr in ((yes, node.body), (no, node.orelse)):
                self.current = block
                value = self.expr(expr)
                self.save(name, value)
                self.drop(value)
                self.jump(join)
            self.current = join
            out = self.load(name, line)
            self.emit("clear", data=name, result=False, raises=False)
            return out
        if isinstance(node, ast.BoolOp):
            name = self.hidden()
            join = self.block()
            self.value_to(node, name, join)
            self.current = join
            out = self.load(name, line)
            self.emit("clear", data=name, result=False, raises=False)
            return out
        raise AssertionError(f"unclassified expression: {type(node).__name__}")

    def assign(self, target, value):
        if isinstance(target, ast.Name):
            self.save(target.id, value, target.lineno)
        elif isinstance(target, ast.Attribute):
            obj = self.expr(target.value)
            self.emit("setattr", [obj, value], data=target.attr, result=False, line=target.lineno)
            self.drop(obj)
        elif isinstance(target, ast.Subscript):
            obj, key = self.expr(target.value), self.expr(target.slice)
            self.emit("setitem", [obj, key, value], result=False, line=target.lineno)
            self.drop(obj)
            self.drop(key)
        else:
            raise AssertionError(type(target).__name__)

    def statements(self, nodes):
        for node in nodes:
            if self.current.terminator is not None:
                break
            if isinstance(node, ast.Expr):
                self.drop(self.expr(node.value))
            elif isinstance(node, ast.Assign):
                value = self.expr(node.value)
                for target in node.targets:
                    self.assign(target, value)
                self.drop(value)
            elif isinstance(node, ast.AugAssign):
                left = self.load(node.target.id, node.lineno)
                right = self.expr(node.value)
                value = self.emit(
                    "inplace", [left, right], data=BINARY[type(node.op)], line=node.lineno
                )
                self.drop(left)
                self.drop(right)
                self.save(node.target.id, value, node.lineno)
                self.drop(value)
            elif isinstance(node, ast.FunctionDef):
                value = self.emit("function", data=self.functions[id(node)], line=node.lineno)
                self.save(node.name, value, node.lineno)
                self.drop(value)
            elif isinstance(node, ast.Return):
                value = self.expr(node.value) if node.value is not None else self.constant(None)
                self.current.terminator = Terminator("return", [value])
            elif isinstance(node, ast.If):
                yes, no, join = self.block(), self.block(), self.block()
                self.condition(node.test, yes, no)
                for block, body in ((yes, node.body), (no, node.orelse)):
                    self.current = block
                    self.statements(body)
                    self.jump(join)
                self.current = join
            elif isinstance(node, (ast.For, ast.While)):
                iterator_slot = None
                if isinstance(node, ast.For):
                    iterable = self.expr(node.iter)
                    iterator = self.emit("iter", [iterable], line=node.lineno)
                    self.drop(iterable)
                    iterator_slot = self.hidden()
                    self.save(iterator_slot, iterator)
                    self.drop(iterator)
                header, body, otherwise, exit_ = (
                    self.block(),
                    self.block(),
                    self.block(),
                    self.block(),
                )
                self.jump(header)
                self.current = header
                if iterator_slot is not None:
                    iterator = self.load(iterator_slot)
                    item = self.reg()
                    self.current.terminator = Terminator(
                        "next", [iterator], [body.name, otherwise.name], item, "error"
                    )
                    self.current = body
                    self.assign(node.target, item)
                    self.drop(item)
                else:
                    self.condition(node.test, body, otherwise)
                    self.current = body
                self.loops.append((exit_.name, header.name))
                self.statements(node.body)
                self.loops.pop()
                self.jump(header)
                self.current = otherwise
                if iterator_slot is not None:
                    self.emit("clear", data=iterator_slot, result=False, raises=False)
                self.statements(node.orelse)
                self.jump(exit_)
                self.current = exit_
                if iterator_slot is not None:
                    self.emit("clear", data=iterator_slot, result=False, raises=False)
            elif isinstance(node, (ast.Break, ast.Continue)):
                target = self.loops[-1][0 if isinstance(node, ast.Break) else 1]
                self.current.terminator = Terminator("jump", targets=[target])
            elif not isinstance(node, ast.Pass):
                raise AssertionError(f"unclassified statement: {type(node).__name__}")


def build(tree: ast.Module) -> Program:
    functions = {
        id(node): f"ca_fn_{i}"
        for i, node in enumerate(ast.walk(tree))
        if isinstance(node, ast.FunctionDef)
    }
    items = [Builder(tree, "ca_module", functions).fn]
    items.extend(
        Builder(node, functions[id(node)], functions).fn
        for node in ast.walk(tree)
        if isinstance(node, ast.FunctionDef)
    )
    return Program(items)
