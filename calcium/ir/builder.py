import ast
import sys
import textwrap
import warnings
from types import CodeType

from ..frontend.analysis import BINARY, COMPARE, UNARY
from ..frontend.constants import MISSING, encode, folded
from ..frontend.lowering import incremental_dict
from .kernels import discover_kernel
from .nodes import Block, Function, Instruction, Program, Terminator


class Builder:
    def __init__(
        self,
        node: ast.Module | ast.FunctionDef,
        symbol: str,
        functions: dict[int, str],
        *,
        definition_fallbacks=frozenset(),
        source=None,
        scope_code=None,
    ):
        module = isinstance(node, ast.Module)
        params = (
            []
            if module
            else [a.arg for a in node.args.posonlyargs + node.args.args + node.args.kwonlyargs]
        )
        if not module:
            params += [a.arg for a in (node.args.vararg, node.args.kwarg) if a is not None]
        # Nested functions are classified as fallback before CIR construction.
        locals_ = []
        if not module:
            # CPython's scope allocator supplies the target-version local order;
            # compiling creates a code object but never executes the function.
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", SyntaxWarning)
                scope = (
                    None
                    if scope_code is not None
                    else compile(
                        ast.Module(body=[node], type_ignores=[]),
                        "<scope>",
                        "exec",
                        dont_inherit=True,
                    )
                )
            code = (
                scope_code
                if scope_code is not None
                else next(value for value in scope.co_consts if isinstance(value, CodeType))
            )
            locals_ = list(code.co_varnames)

            class Bindings(ast.NodeVisitor):
                names = set(params)

                def visit_Name(self, item):
                    if isinstance(item.ctx, (ast.Store, ast.Del)):
                        self.names.add(item.id)

                def visit_Import(self, item):
                    self.names.update(a.asname or a.name.split(".")[0] for a in item.names)

                def visit_ImportFrom(self, item):
                    self.names.update(a.asname or a.name for a in item.names)

                def visit_ExceptHandler(self, item):
                    if item.name:
                        self.names.add(item.name)
                    self.generic_visit(item)

                def visit_FunctionDef(self, item):
                    self.names.add(item.name)

                visit_AsyncFunctionDef = visit_FunctionDef
                visit_ClassDef = visit_FunctionDef

                def visit_ListComp(self, item):
                    for child in ast.walk(item):
                        if isinstance(child, ast.NamedExpr):
                            self.names.add(child.target.id)

                visit_DictComp = visit_ListComp
                visit_SetComp = visit_ListComp
                visit_GeneratorExp = visit_ListComp

            bindings = Bindings()
            for statement in node.body:
                bindings.visit(statement)
            locals_ = [name for name in locals_ if name in bindings.names]
        doc = ast.get_docstring(node, clean=False)
        self.fn = Function(
            "<module>" if module else node.name,
            symbol,
            params,
            locals_,
            doc=doc,
            module=module,
            posonly=0 if module else len(node.args.posonlyargs),
            positional=0 if module else len(node.args.posonlyargs + node.args.args),
            kwonly=[] if module else [a.arg for a in node.args.kwonlyargs],
            vararg=None if module or node.args.vararg is None else node.args.vararg.arg,
            kwarg=None if module or node.args.kwarg is None else node.args.kwarg.arg,
        )
        self.functions = functions
        self.definition_fallbacks = definition_fallbacks
        self.source = source
        self.error_target = "error"
        self.cleanups = []
        self.aliases = {}
        self.handler_starts = {}
        self.current = self.block()
        self.loops: list[tuple[str, str, int]] = []
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
        if not module:
            self.fn.kernel = discover_kernel(node, self.fn.locals)
        for label, start in self.handler_starts.items():
            self.fn.exception_handlers[label] = [
                name for name in self.fn.locals[start:] if name.startswith("$")
            ]

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
            Instruction(op, args or [], value, data, line, self.error_target if raises else None)
        )
        return value

    def drop(self, value):
        self.emit("drop", [value], result=False, raises=False)

    def constant(self, value, line=1, site=()):
        # repr is data, never C text; the backend chooses construction APIs.
        return self.emit("constant", data=encode(value, site), line=line)

    def jump(self, block):
        if self.current.terminator is None:
            self.current.terminator = Terminator("jump", targets=[block.name])

    def hidden(self):
        name = f"$tmp{len(self.fn.locals)}"
        self.fn.locals.append(name)
        return name

    def save(self, name, value, line=1):
        self.emit("store", [value], data=self.aliases.get(name, name), result=False, line=line)

    def load(self, name, line=1):
        return self.emit("load", data=self.aliases.get(name, name), line=line)

    def condition(self, node, yes, no):
        """A truth context must not re-test a short-circuited user object."""
        if isinstance(node, ast.Compare) and len(node.ops) > 1:
            self.comparison(node, yes, no)
        elif isinstance(node, ast.BoolOp):
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
                "branch", [value], [yes.name, no.name], error=self.error_target
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
                "branch", [value], [yes.name, no.name], error=self.error_target
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
            return self.constant(value, line, (line, node.col_offset))
        if isinstance(node, ast.Constant):
            return self.constant(node.value, line)
        if isinstance(node, ast.Name):
            return self.load(node.id, line)
        if isinstance(node, ast.NamedExpr):
            value = self.expr(node.value)
            self.save(node.target.id, value, line)
            return value
        if isinstance(node, ast.JoinedStr):
            args = [self.expr(item) for item in node.values]
            out = self.emit("join_string", args, line=line)
            for arg in args:
                self.drop(arg)
            return out
        if isinstance(node, ast.FormattedValue):
            value = self.expr(node.value)
            # FORMAT_VALUE in 3.12 converts after evaluating the format spec;
            # CONVERT_VALUE in 3.13+ runs before the spec. Callbacks observe it.
            early_spec = (
                self.expr(node.format_spec)
                if node.format_spec is not None and sys.version_info < (3, 13)
                else None
            )
            if node.conversion != -1:
                converted = self.emit("convert_string", [value], data=node.conversion, line=line)
                self.drop(value)
                value = converted
            spec = (
                early_spec
                if early_spec is not None
                else self.expr(node.format_spec)
                if node.format_spec
                else self.constant("")
            )
            out = self.emit("format", [value, spec], line=line)
            self.drop(value)
            self.drop(spec)
            return out
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
            if len(node.ops) > 1:
                return self.comparison(node)
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
            if any(isinstance(a, ast.Starred) for a in node.args) or any(
                k.arg is None for k in node.keywords
            ):
                return self.expanded_call(node)
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
        if isinstance(node, (ast.ListComp, ast.SetComp, ast.DictComp)):
            return self.comprehension(node)
        if isinstance(node, (ast.List, ast.Tuple, ast.Set, ast.Dict)):
            if isinstance(node, ast.Set) and any(
                isinstance(item, ast.Starred) for item in node.elts
            ):
                result = self.emit("container", data="set", line=line)
                chunk = []

                def flush():
                    if chunk:
                        values = [self.expr(item) for item in chunk]
                        temporary = self.emit("container", values, data="set", line=line)
                        for value in values:
                            self.drop(value)
                        self.emit("set_update", [result, temporary], result=False, line=line)
                        self.drop(temporary)
                        chunk.clear()

                for item in node.elts:
                    if isinstance(item, ast.Starred):
                        flush()
                        value = self.expr(item.value)
                        self.emit("set_update", [result, value], result=False, line=line)
                        self.drop(value)
                    else:
                        chunk.append(item)
                flush()
                return result
            if isinstance(node, ast.Dict) and (
                any(key is None for key in node.keys) or incremental_dict(node)
            ):
                result = self.emit("container", data="dict", line=line)
                for key, value_node in zip(node.keys, node.values):
                    if key is None:
                        mapping = self.expr(value_node)
                        self.emit("dict_merge", [result, mapping], result=False, line=line)
                        self.drop(mapping)
                    else:
                        key_value, value = self.expr(key), self.expr(value_node)
                        self.emit("setitem", [result, key_value, value], result=False, line=line)
                        self.drop(key_value)
                        self.drop(value)
                return result
            if isinstance(node, (ast.List, ast.Tuple)) and any(
                isinstance(item, ast.Starred) for item in node.elts
            ):
                result = self.emit("container", data="list", line=line)
                for item in node.elts:
                    starred = isinstance(item, ast.Starred)
                    value = self.expr(item.value if starred else item)
                    self.emit(
                        "args_extend" if starred else "args_append",
                        [result, value],
                        result=False,
                        line=line,
                    )
                    self.drop(value)
                if isinstance(node, ast.Tuple):
                    converted = self.emit("to_tuple", [result], line=line)
                    self.drop(result)
                    return converted
                return result
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

    def expanded_call(self, node):
        function = self.expr(node.func)
        positional = self.emit("container", data="list", line=node.lineno)
        for arg in node.args:
            star = isinstance(arg, ast.Starred)
            value = self.expr(arg.value if star else arg)
            self.emit(
                "args_extend" if star else "args_append",
                [positional, value],
                result=False,
                line=node.lineno,
            )
            self.drop(value)
        keywords = self.emit("container", data="dict", line=node.lineno)
        for keyword in node.keywords:
            value = self.expr(keyword.value)
            self.emit(
                "kwargs_merge" if keyword.arg is None else "kwargs_set",
                [keywords, value],
                data=keyword.arg,
                result=False,
                line=node.lineno,
            )
            self.drop(value)
        result = self.emit("call_ex", [function, positional, keywords], line=node.lineno)
        self.drop(function)
        self.drop(positional)
        self.drop(keywords)
        return result

    def comparison(self, node, yes=None, no=None):
        """Evaluate each operand once and retain rich-comparison return objects."""
        carry, result = self.hidden(), self.hidden()
        end = self.block()
        left = self.expr(node.left)
        self.save(carry, left)
        self.drop(left)
        for index, (operator, operand) in enumerate(zip(node.ops, node.comparators)):
            left, right = self.load(carry), self.expr(operand)
            value = self.emit(
                "compare", [left, right], data=COMPARE[type(operator)], line=node.lineno
            )
            self.save(carry, right)
            self.drop(left)
            self.drop(right)
            final = index == len(node.ops) - 1
            if final and yes is None:
                self.save(result, value)
                self.drop(value)
                self.jump(end)
            else:
                onward = self.block() if not final else yes
                # Both condition exits clear the carried operand before branching
                # to the caller's blocks. Value contexts keep the actual result.
                if yes is None:
                    self.save(result, value)
                self.current.terminator = Terminator(
                    "branch", [value], [onward.name, end.name], error=self.error_target
                )
                if not final:
                    self.current = onward
                    self.emit("clear", data=result, result=False, raises=False)
        if yes is not None:
            # Redirect the final true edge through cleanup as well.
            true_cleanup = self.block()
            self.current.terminator.targets[0] = true_cleanup.name
            self.current = true_cleanup
            self.emit("clear", data=carry, result=False, raises=False)
            self.jump(yes)
            self.current = end
            self.emit("clear", data=carry, result=False, raises=False)
            self.jump(no)
            return None
        self.current = end
        self.emit("clear", data=carry, result=False, raises=False)
        value = self.load(result, node.lineno)
        self.emit("clear", data=result, result=False, raises=False)
        return value

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
        elif isinstance(target, (ast.Tuple, ast.List)):
            star = next(
                (i for i, item in enumerate(target.elts) if isinstance(item, ast.Starred)), None
            )
            unpacked = self.emit(
                "unpack_ex" if star is not None else "unpack",
                [value],
                data={"before": star, "after": len(target.elts) - star - 1}
                if star is not None
                else len(target.elts),
                line=target.lineno,
            )
            for index, item in enumerate(target.elts):
                part = self.emit("tuple_item", [unpacked], data=index, line=target.lineno)
                self.assign(item.value if isinstance(item, ast.Starred) else item, part)
                self.drop(part)
            self.drop(unpacked)
        else:
            raise AssertionError(type(target).__name__)

    def statements(self, nodes):
        for node in nodes:
            if self.current.terminator is not None:
                break
            if id(node) in self.definition_fallbacks:
                first = min([node.lineno, *(d.lineno for d in node.decorator_list)])
                if self.source is None:
                    text = ast.unparse(node)
                else:
                    text = textwrap.dedent(
                        "\n".join(self.source.splitlines()[first - 1 : node.end_lineno])
                    )
                self.emit(
                    "python_definition",
                    data="\n" * (first - 1) + text + "\n",
                    result=False,
                    line=first,
                )
            elif isinstance(node, ast.Expr):
                self.drop(self.expr(node.value))
            elif isinstance(node, ast.Assign):
                value = self.expr(node.value)
                for target in node.targets:
                    self.assign(target, value)
                self.drop(value)
            elif isinstance(node, ast.Import):
                for alias in node.names:
                    value = self.emit(
                        "import",
                        data={"name": alias.name, "fromlist": [], "level": 0},
                        line=node.lineno,
                    )
                    if alias.asname:
                        for part in alias.name.split(".")[1:]:
                            child = self.emit("import_from", [value], data=part, line=node.lineno)
                            self.drop(value)
                            value = child
                    self.save(alias.asname or alias.name.split(".")[0], value, node.lineno)
                    self.drop(value)
            elif isinstance(node, ast.ImportFrom):
                module = self.emit(
                    "import",
                    data={
                        "name": node.module or "",
                        "fromlist": [a.name for a in node.names],
                        "level": node.level,
                    },
                    line=node.lineno,
                )
                for alias in node.names:
                    value = self.emit("import_from", [module], data=alias.name, line=node.lineno)
                    self.save(alias.asname or alias.name, value, node.lineno)
                    self.drop(value)
                self.drop(module)
            elif isinstance(node, ast.AugAssign):
                target = node.target
                obj = key = None
                if isinstance(target, ast.Name):
                    left = self.load(target.id, node.lineno)
                elif isinstance(target, ast.Attribute):
                    obj = self.expr(target.value)
                    left = self.emit("attribute", [obj], data=target.attr, line=node.lineno)
                else:
                    obj, key = self.expr(target.value), self.expr(target.slice)
                    left = self.emit("getitem", [obj, key], line=node.lineno)
                right = self.expr(node.value)
                value = self.emit(
                    "inplace", [left, right], data=BINARY[type(node.op)], line=node.lineno
                )
                self.drop(left)
                self.drop(right)
                if obj is None:
                    self.save(target.id, value, node.lineno)
                elif key is None:
                    self.emit(
                        "setattr", [obj, value], data=target.attr, result=False, line=node.lineno
                    )
                    self.drop(obj)
                else:
                    self.emit("setitem", [obj, key, value], result=False, line=node.lineno)
                    self.drop(obj)
                    self.drop(key)
                self.drop(value)
            elif isinstance(node, ast.FunctionDef):
                defaults = [self.expr(v) for v in node.args.defaults]
                keywords = [
                    (arg.arg, v)
                    for arg, v in zip(node.args.kwonlyargs, node.args.kw_defaults)
                    if v is not None
                ]
                kwvalues = [self.expr(v) for _, v in keywords]
                value = self.emit(
                    "function",
                    defaults + kwvalues,
                    data={
                        "symbol": self.functions[id(node)],
                        "posdefaults": len(defaults),
                        "kwdefaults": [name for name, _ in keywords],
                    },
                    line=node.lineno,
                )
                for arg in defaults + kwvalues:
                    self.drop(arg)
                self.save(node.name, value, node.lineno)
                self.drop(value)
            elif isinstance(node, ast.Return):
                value = self.expr(node.value) if node.value is not None else self.constant(None)
                self.transfer("return", value=value)
            elif isinstance(node, ast.Try):
                self.try_statement(node)
            elif isinstance(node, ast.With):
                self.with_statement(node.items, node.body)
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
                        "next", [iterator], [body.name, otherwise.name], item, self.error_target
                    )
                    self.current = body
                    self.assign(node.target, item)
                    self.drop(item)
                else:
                    self.condition(node.test, body, otherwise)
                    self.current = body
                self.loops.append((exit_.name, header.name, len(self.cleanups)))
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
                self.transfer("jump", target=target, depth=self.loops[-1][2])
            elif isinstance(node, ast.Assert):
                okay, failed = self.block(), self.block()
                self.condition(node.test, okay, failed)
                self.current = failed
                args = [self.expr(node.msg)] if node.msg is not None else []
                self.emit("assert_fail", args, result=False, line=node.lineno)
                self.current.terminator = Terminator("error", error=self.error_target)
                self.current = okay
            elif isinstance(node, ast.Raise):
                args = [self.expr(node.exc)] if node.exc is not None else []
                if node.cause is not None:
                    args.append(self.expr(node.cause))
                self.emit("raise", args, result=False, line=node.lineno)
                self.current.terminator = Terminator("error", error=self.error_target)
            elif isinstance(node, ast.Delete):
                for target in node.targets:
                    self.delete(target)
            elif isinstance(node, ast.Global):
                pass
            elif not isinstance(node, ast.Pass):
                raise AssertionError(f"unclassified statement: {type(node).__name__}")

    def comprehension(self, node):
        line = node.lineno
        first = self.expr(node.generators[0].iter)
        iterator = self.emit("iter", [first], line=line)
        self.drop(first)
        iterator_slot = self.hidden()
        self.save(iterator_slot, iterator)
        self.drop(iterator)
        aliases = self.aliases.copy()
        allocated = []
        targets = set()
        for generator in node.generators:
            for item in ast.walk(generator.target):
                if isinstance(item, ast.Name) and item.id not in targets:
                    targets.add(item.id)
                    self.aliases[item.id] = self.hidden()
                    allocated.append(self.aliases[item.id])
        kind = (
            "dict"
            if isinstance(node, ast.DictComp)
            else "set"
            if isinstance(node, ast.SetComp)
            else "list"
        )
        result_slot = self.hidden()
        result = self.emit("container", data=kind, line=line)
        self.save(result_slot, result)
        self.drop(result)

        def loop(index):
            generator = node.generators[index]
            slot = iterator_slot if index == 0 else self.hidden()
            if index:
                value = self.expr(generator.iter)
                it = self.emit("iter", [value], line=line)
                self.drop(value)
                self.save(slot, it)
                self.drop(it)
            header, body, end = self.block(), self.block(), self.block()
            self.jump(header)
            self.current = header
            it = self.load(slot)
            item = self.reg()
            self.current.terminator = Terminator(
                "next", [it], [body.name, end.name], item, self.error_target
            )
            self.current = body
            self.assign(generator.target, item)
            self.drop(item)
            for predicate in generator.ifs:
                onward = self.block()
                self.condition(predicate, onward, header)
                self.current = onward
            if index + 1 < len(node.generators):
                loop(index + 1)
            else:
                target = self.load(result_slot)
                if kind == "dict":
                    key, value = self.expr(node.key), self.expr(node.value)
                    self.emit("setitem", [target, key, value], result=False, line=line)
                    self.drop(key)
                else:
                    value = self.expr(node.elt)
                    self.emit(
                        "set_add" if kind == "set" else "args_append",
                        [target, value],
                        result=False,
                        line=line,
                    )
                self.drop(value)
                self.drop(target)
            self.jump(header)
            self.current = end
            self.emit("clear", data=slot, result=False, raises=False)

        loop(0)
        for name in reversed(allocated):
            self.emit("clear", data=name, result=False, raises=False)
        self.aliases = aliases
        result = self.load(result_slot, line)
        self.emit("clear", data=result_slot, result=False, raises=False)
        return result

    def handler(self):
        block = self.block()
        self.handler_starts[block.name] = len(self.fn.locals)
        return block

    def clear_hidden(self, name):
        self.emit("clear", data=name, result=False, raises=False)

    def exception_state(self):
        previous, caught = self.hidden(), self.hidden()
        value = self.emit("exception_take")
        self.save(caught, value)
        old = self.emit("handled_get")
        self.save(previous, old)
        self.drop(old)
        self.emit("handled_set", [value], result=False, raises=False)
        self.drop(value)
        return {"kind": "exception", "previous": previous, "caught": caught, "binding": None}

    def finish_exception(self, action):
        pending = self.emit("error_get", raises=False)
        if action["binding"]:
            self.emit("clear_binding", data=action["binding"], result=False, raises=False)
        previous = self.load(action["previous"])
        self.emit("handled_set", [previous], result=False, raises=False)
        self.drop(previous)
        self.clear_hidden(action["previous"])
        self.clear_hidden(action["caught"])
        self.emit("error_set", [pending], result=False, raises=False)
        self.drop(pending)

    def cleanup(self, action):
        if action["kind"] == "exception":
            self.finish_exception(action)
        elif action["kind"] == "finally":
            self.statements(action["body"])
        else:
            exit_function = self.load(action["exit"])
            none = self.constant(None)
            result = self.emit(
                "call", [exit_function, none, none, none], data={"positional": 3, "keywords": []}
            )
            self.drop(result)
            self.drop(none)
            self.drop(exit_function)
            self.clear_hidden(action["exit"])

    def transfer(self, kind, *, value=None, target=None, depth=0):
        pending = None
        if value is not None:
            pending = self.hidden()
            self.save(pending, value)
            self.drop(value)
        actions, error = self.cleanups, self.error_target
        for index in reversed(range(depth, len(actions))):
            action = actions[index]
            self.cleanups = actions[:index]
            self.error_target = action["outer"]
            self.cleanup(action)
            if self.current.terminator is not None:
                self.cleanups, self.error_target = actions, error
                return
        self.cleanups, self.error_target = actions, error
        if pending:
            value = self.load(pending)
            self.clear_hidden(pending)
        self.current.terminator = Terminator(
            kind, [value] if value is not None else [], [target] if target else []
        )

    def try_statement(self, node):
        if node.finalbody:
            dispatch, end = self.handler(), self.block()
            outer = self.error_target
            action = {"kind": "finally", "body": node.finalbody, "outer": outer}
            self.cleanups.append(action)
            self.error_target = dispatch.name
            inner = ast.Try(
                body=node.body, handlers=node.handlers, orelse=node.orelse, finalbody=[]
            )
            if node.handlers:
                self.try_statement(inner)
            else:
                self.statements(node.body)
            self.cleanups.pop()
            self.error_target = outer
            if self.current.terminator is None:
                self.statements(node.finalbody)
                self.jump(end)
            self.current = dispatch
            state = self.exception_state()
            state["outer"] = outer
            failure = self.handler()
            self.error_target = failure.name
            self.cleanups.append(state)
            self.statements(node.finalbody)
            self.cleanups.pop()
            self.error_target = outer
            if self.current.terminator is None:
                exception = self.load(state["caught"])
                self.finish_exception(state)
                self.emit("exception_raise", [exception], result=False, raises=False)
                self.drop(exception)
                self.current.terminator = Terminator("error", error=outer)
            self.current = failure
            self.finish_exception(state)
            self.current.terminator = Terminator("error", error=outer)
            self.current = end
            return
        dispatch, otherwise, end = self.handler(), self.block(), self.block()
        outer = self.error_target
        self.error_target = dispatch.name
        self.statements(node.body)
        self.jump(otherwise)
        self.error_target = outer
        self.current = otherwise
        self.statements(node.orelse)
        self.jump(end)
        self.current = dispatch
        state = self.exception_state()
        state["outer"] = outer
        failure = self.handler()
        self.error_target = failure.name
        for handler in node.handlers:
            body, next_handler = self.block(), self.block()
            if handler.type is not None:
                exception, expected = self.load(state["caught"]), self.expr(handler.type)
                matches = self.emit("exception_match", [exception, expected])
                self.drop(exception)
                self.drop(expected)
                self.current.terminator = Terminator(
                    "branch", [matches], [body.name, next_handler.name], error=self.error_target
                )
            else:
                self.jump(body)
            self.current = body
            current_state = dict(state, binding=handler.name)
            handler_failure = self.handler()
            self.error_target = handler_failure.name
            if handler.name:
                value = self.load(state["caught"])
                self.save(handler.name, value)
                self.drop(value)
            self.cleanups.append(current_state)
            self.statements(handler.body)
            self.cleanups.pop()
            self.error_target = outer
            if self.current.terminator is None:
                self.finish_exception(current_state)
                self.jump(end)
            self.current = handler_failure
            self.finish_exception(current_state)
            self.current.terminator = Terminator("error", error=outer)
            self.current = next_handler
            self.error_target = failure.name
        exception = self.load(state["caught"])
        self.finish_exception(state)
        self.emit("exception_raise", [exception], result=False, raises=False)
        self.drop(exception)
        self.current.terminator = Terminator("error", error=outer)
        self.current = failure
        self.error_target = outer
        self.finish_exception(state)
        self.current.terminator = Terminator("error", error=outer)
        self.current = end

    def with_statement(self, items, body):
        item, rest = items[0], items[1:]
        manager = self.expr(item.context_expr)
        # CPython 3.14's LOAD_SPECIAL sequence changed observable descriptor
        # binding order from 3.12/3.13's BEFORE_WITH implementation.
        if sys.version_info >= (3, 14):
            exit_function = self.emit("special", [manager], data="__exit__")
            enter_function = self.emit("special", [manager], data="__enter__")
        else:
            enter_function = self.emit("special", [manager], data="__enter__")
            exit_function = self.emit("special", [manager], data="__exit__")
        self.drop(manager)
        exit_slot = self.hidden()
        self.save(exit_slot, exit_function)
        self.drop(exit_function)
        entered = self.emit("call", [enter_function], data={"positional": 0, "keywords": []})
        self.drop(enter_function)
        dispatch, end = self.handler(), self.block()
        outer = self.error_target
        action = {"kind": "with", "exit": exit_slot, "outer": outer}
        self.cleanups.append(action)
        self.error_target = dispatch.name
        if item.optional_vars is not None:
            self.assign(item.optional_vars, entered)
        self.drop(entered)
        if rest:
            self.with_statement(rest, body)
        else:
            self.statements(body)
        self.cleanups.pop()
        self.error_target = outer
        if self.current.terminator is None:
            self.cleanup(action)
            self.jump(end)
        self.current = dispatch
        state = self.exception_state()
        state["outer"] = outer
        failure = self.handler()
        self.error_target = failure.name
        self.cleanups.append(state)
        exception = self.load(state["caught"])
        exit_function = self.load(exit_slot)
        result = self.emit("with_exit", [exit_function, exception])
        self.drop(exit_function)
        self.drop(exception)
        suppressed, propagated = self.block(), self.block()
        self.current.terminator = Terminator(
            "branch", [result], [suppressed.name, propagated.name], error=failure.name
        )
        self.cleanups.pop()
        self.error_target = outer
        self.current = suppressed
        self.finish_exception(state)
        self.clear_hidden(exit_slot)
        self.jump(end)
        self.current = propagated
        exception = self.load(state["caught"])
        self.finish_exception(state)
        self.clear_hidden(exit_slot)
        self.emit("exception_raise", [exception], result=False, raises=False)
        self.drop(exception)
        self.current.terminator = Terminator("error", error=outer)
        self.current = failure
        self.finish_exception(state)
        self.clear_hidden(exit_slot)
        self.current.terminator = Terminator("error", error=outer)
        self.current = end

    def delete(self, target):
        if isinstance(target, ast.Name):
            self.emit("delete", data=target.id, result=False, line=target.lineno)
        elif isinstance(target, (ast.Tuple, ast.List)):
            for item in target.elts:
                self.delete(item)
        else:
            obj = self.expr(target.value)
            args = [obj]
            if isinstance(target, ast.Subscript):
                args.append(self.expr(target.slice))
            self.emit(
                "delattr" if isinstance(target, ast.Attribute) else "delitem",
                args,
                data=target.attr if isinstance(target, ast.Attribute) else None,
                result=False,
                line=target.lineno,
            )
            for arg in args:
                self.drop(arg)


def build(tree: ast.Module, *, definition_fallbacks=frozenset(), source=None) -> Program:
    def native_nodes(node):
        if id(node) in definition_fallbacks:
            return
        yield node
        for child in ast.iter_child_nodes(node):
            yield from native_nodes(child)

    nodes = list(native_nodes(tree))
    functions = {
        id(node): f"ca_fn_{i}" for i, node in enumerate(nodes) if isinstance(node, ast.FunctionDef)
    }
    options = {"definition_fallbacks": definition_fallbacks, "source": source}
    items = [Builder(tree, "ca_module", functions, **options).fn]
    items.extend(
        Builder(node, functions[id(node)], functions, **options).fn
        for node in nodes
        if isinstance(node, ast.FunctionDef)
    )
    return Program(items, "mixed" if definition_fallbacks else "native")
