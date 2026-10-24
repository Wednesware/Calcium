import ast
import sys
import textwrap
from types import CodeType

from ..frontend.analysis import BINARY, COMPARE, UNARY
from ..frontend.constants import MISSING, encode, folded
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
            posonly=0 if module else len(node.args.posonlyargs),
            positional=0 if module else len(node.args.posonlyargs + node.args.args),
            kwonly=[] if module else [a.arg for a in node.args.kwonlyargs],
            vararg=None if module or node.args.vararg is None else node.args.vararg.arg,
            kwarg=None if module or node.args.kwarg is None else node.args.kwarg.arg,
        )
        self.functions = functions
        self.definition_fallbacks = definition_fallbacks
        self.source = source
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
        self.emit("store", [value], data=name, result=False, line=line)

    def load(self, name, line=1):
        return self.emit("load", data=name, line=line)

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
        if isinstance(node, (ast.List, ast.Tuple, ast.Set, ast.Dict)):
            if isinstance(node, ast.Dict) and any(key is None for key in node.keys):
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
                    "branch", [value], [onward.name, end.name], error="error"
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
            elif isinstance(node, ast.Assert):
                okay, failed = self.block(), self.block()
                self.condition(node.test, okay, failed)
                self.current = failed
                args = [self.expr(node.msg)] if node.msg is not None else []
                self.emit("assert_fail", args, result=False, line=node.lineno)
                self.current.terminator = Terminator("error")
                self.current = okay
            elif isinstance(node, ast.Raise):
                args = [self.expr(node.exc)] if node.exc is not None else []
                if node.cause is not None:
                    args.append(self.expr(node.cause))
                self.emit("raise", args, result=False, line=node.lineno)
                self.current.terminator = Terminator("error")
            elif isinstance(node, ast.Delete):
                for target in node.targets:
                    self.delete(target)
            elif isinstance(node, ast.Global):
                pass
            elif not isinstance(node, ast.Pass):
                raise AssertionError(f"unclassified statement: {type(node).__name__}")

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
