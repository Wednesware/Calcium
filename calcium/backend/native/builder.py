"""Bonemarrow CIR extension: lexical cells, native classes and decorators."""

import ast
import copy
import inspect
import sys
import warnings
from types import CodeType

from ...ir.builder import Builder
from ...ir.nodes import Program, Terminator


class NativeBuilder(Builder):
    def __init__(self, node, symbol, functions, *, codes, dynamic, generators):
        self.class_slots = []
        self.native_cells = set()
        self.generators = generators
        self.codes = codes
        self.dynamic = dynamic
        code = codes.get(id(node))
        super().__init__(node, symbol, functions, scope_code=code)
        if code:
            hidden = [n for n in self.fn.locals if n.startswith("$")]
            declared = list(
                dict.fromkeys(
                    [*(n for n in self.fn.locals if not n.startswith("$")), *code.co_cellvars]
                )
            )
            self.fn.locals = declared + hidden
            self.fn.cleanup_order = list(reversed(self.fn.locals))
            self.fn.native = {
                "freevars": list(code.co_freevars),
                "cellvars": list(dict.fromkeys([*code.co_cellvars, *sorted(self.native_cells)])),
                "dynamic": dynamic.get(id(node), True),
                "coroutine": bool(code.co_flags & inspect.CO_COROUTINE),
                "generator": bool(code.co_flags & (inspect.CO_GENERATOR | inspect.CO_COROUTINE)),
            }
            if (
                code.co_freevars
                or code.co_cellvars
                or code.co_flags & (inspect.CO_GENERATOR | inspect.CO_COROUTINE)
            ):
                self.fn.kernel = None
        else:
            self.fn.native = {
                "freevars": [],
                "cellvars": sorted(self.native_cells),
                "dynamic": False,
            }

    def load(self, name, line=1):
        if self.class_slots and not name.startswith("$"):
            owner = super().load(self.class_slots[-1], line)
            raw = self.emit("native_raw_load", data=name, line=line)
            out = self.emit("native_class_load", [owner, raw], data=name, line=line)
            self.drop(owner)
            self.drop(raw)
            return out
        return super().load(name, line)

    def save(self, name, value, line=1):
        if self.class_slots and not name.startswith("$"):
            owner = super().load(self.class_slots[-1], line)
            self.emit("setattr", [owner, value], data=name, result=False, line=line)
            self.drop(owner)
        else:
            super().save(name, value, line)

    def function(self, node):
        decorators = [self.expr(d) for d in node.decorator_list]
        defaults = [self.expr(d) for d in node.args.defaults]
        keywords = [
            (a.arg, d) for a, d in zip(node.args.kwonlyargs, node.args.kw_defaults) if d is not None
        ]
        kwvalues = [self.expr(d) for _, d in keywords]
        captures = []
        code = self.codes.get(id(node))
        for name in code.co_freevars if code else []:
            binding = self.aliases.get(name, name)
            if name in self.aliases:
                self.native_cells.add(binding)
            captures.append(binding)
        value = self.emit(
            "function",
            defaults + kwvalues,
            data={
                "symbol": self.functions[id(node)],
                "posdefaults": len(defaults),
                "kwdefaults": [n for n, _ in keywords],
                "captures": captures,
            },
            line=node.lineno,
        )
        for arg in defaults + kwvalues:
            self.drop(arg)
        for decorator in reversed(decorators):
            result = self.emit(
                "call", [decorator, value], data={"positional": 1, "keywords": []}, line=node.lineno
            )
            self.drop(decorator)
            self.drop(value)
            value = result
        return value

    def expr(self, node):
        if isinstance(node, ast.Await):
            value = self.expr(node.value)
            result = self.emit("native_await", [value], line=node.lineno)
            self.drop(value)
            return result
        if isinstance(node, ast.Yield):
            value = self.expr(node.value) if node.value is not None else self.constant(None)
            result = self.emit("native_yield", [value], line=node.lineno)
            self.drop(value)
            return result
        if isinstance(node, ast.GeneratorExp):
            function = self.function(self.generators[id(node)])
            value = self.expr(node.generators[0].iter)
            iterator = self.emit("iter", [value], line=node.lineno)
            self.drop(value)
            result = self.emit(
                "call",
                [function, iterator],
                data={"positional": 1, "keywords": []},
                line=node.lineno,
            )
            self.drop(function)
            self.drop(iterator)
            return result
        if isinstance(node, ast.Lambda):
            return self.function(node)
        return super().expr(node)

    def statements(self, nodes):
        for node in nodes:
            if self.current.terminator is not None:
                break
            if isinstance(node, ast.ClassDef):
                decorators = [self.expr(d) for d in node.decorator_list]
                bases = [self.expr(b) for b in node.bases]
                value = self.emit("native_class", bases, data=node.name, line=node.lineno)
                for base in bases:
                    self.drop(base)
                slot = self.hidden()
                super().save(slot, value)
                self.drop(value)
                self.class_slots.append(slot)
                module = super().load("__name__", node.lineno)
                self.save("__module__", module, node.lineno)
                self.drop(module)
                doc = self.constant(ast.get_docstring(node, clean=False), node.lineno)
                self.save("__doc__", doc, node.lineno)
                self.drop(doc)
                if any(
                    isinstance(n, ast.AnnAssign) and isinstance(n.target, ast.Name)
                    for n in node.body
                ):
                    annotations = self.emit("container", data="dict", line=node.lineno)
                    self.save("__annotations__", annotations)
                    self.drop(annotations)
                self.statements(node.body)
                self.class_slots.pop()
                value = super().load(slot)
                self.emit("clear", data=slot, result=False, raises=False)
                for decorator in reversed(decorators):
                    result = self.emit(
                        "call",
                        [decorator, value],
                        data={"positional": 1, "keywords": []},
                        line=node.lineno,
                    )
                    self.drop(decorator)
                    self.drop(value)
                    value = result
                self.save(node.name, value, node.lineno)
                self.drop(value)
            elif isinstance(node, ast.AsyncWith):
                self.with_statement(node.items, node.body, asynchronous=True)
            elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                value = self.function(node)
                self.save(node.name, value, node.lineno)
                self.drop(value)
            elif isinstance(node, ast.AnnAssign):
                if self.class_slots and isinstance(node.target, ast.Name):
                    mapping = self.load("__annotations__", node.lineno)
                    key = self.constant(node.target.id, node.lineno)
                    annotation = self.constant(ast.unparse(node.annotation), node.lineno)
                    self.emit("setitem", [mapping, key, annotation], result=False, line=node.lineno)
                    for arg in [mapping, key, annotation]:
                        self.drop(arg)
                if node.value is not None:
                    value = self.expr(node.value)
                    self.assign(node.target, value)
                    self.drop(value)
            elif isinstance(node, (ast.Global, ast.Nonlocal)):
                pass
            else:
                super().statements([node])

    def cleanup(self, action):
        if action["kind"] != "asyncwith":
            return super().cleanup(action)
        exit_function = self.load(action["exit"])
        none = self.constant(None)
        result = self.emit(
            "call", [exit_function, none, none, none], data={"positional": 3, "keywords": []}
        )
        awaited = self.emit("native_await", [result])
        for value in [awaited, result, none, exit_function]:
            self.drop(value)
        self.clear_hidden(action["exit"])

    def with_statement(self, items, body, asynchronous=False):
        item, rest = items[0], items[1:]
        manager = self.expr(item.context_expr)
        # CPython 3.14's LOAD_SPECIAL sequence changed observable descriptor
        # binding order from 3.12/3.13's BEFORE_WITH implementation.
        if sys.version_info >= (3, 14):
            exit_function = self.emit(
                "special", [manager], data="__aexit__" if asynchronous else "__exit__"
            )
            enter_function = self.emit(
                "special", [manager], data="__aenter__" if asynchronous else "__enter__"
            )
        else:
            enter_function = self.emit(
                "special", [manager], data="__aenter__" if asynchronous else "__enter__"
            )
            exit_function = self.emit(
                "special", [manager], data="__aexit__" if asynchronous else "__exit__"
            )
        self.drop(manager)
        exit_slot = self.hidden()
        self.save(exit_slot, exit_function)
        self.drop(exit_function)
        entered = self.emit("call", [enter_function], data={"positional": 0, "keywords": []})
        self.drop(enter_function)
        if asynchronous:
            awaited = self.emit("native_await", [entered])
            self.drop(entered)
            entered = awaited
        dispatch, end = self.handler(), self.block()
        outer = self.error_target
        action = {
            "kind": "asyncwith" if asynchronous else "with",
            "exit": exit_slot,
            "outer": outer,
        }
        self.cleanups.append(action)
        self.error_target = dispatch.name
        if item.optional_vars is not None:
            self.assign(item.optional_vars, entered)
        self.drop(entered)
        if rest:
            self.with_statement(rest, body, asynchronous=asynchronous)
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
        if asynchronous:
            awaited = self.emit("native_await", [result])
            self.drop(result)
            result = awaited
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


def build(tree):
    tree = copy.deepcopy(tree)
    nodes = []
    dynamic = {}
    code_map = {}
    generators = {}

    def walk(node, parent=None):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.GeneratorExp)):
            nodes.append(node)
            dynamic[id(node)] = not isinstance(parent, ast.Module)
        for child in ast.iter_child_nodes(node):
            walk(child, node)

    walk(tree)
    with warnings.catch_warnings(record=True):
        warnings.simplefilter("always", SyntaxWarning)
        code = compile(tree, "<bonemarrow>", "exec", dont_inherit=True)
    codes = {}

    def collect(c):
        for child in c.co_consts:
            if isinstance(child, CodeType):
                codes.setdefault((child.co_name, child.co_firstlineno), []).append(child)
                collect(child)

    collect(code)
    for node in nodes:
        first = min([node.lineno, *(d.lineno for d in getattr(node, "decorator_list", []))])
        key = (
            "<lambda>"
            if isinstance(node, ast.Lambda)
            else "<genexpr>"
            if isinstance(node, ast.GeneratorExp)
            else node.name,
            first,
        )
        choices = codes.get(key, [])
        if choices:
            code_map[id(node)] = choices.pop(0)
    functions = {id(n): f"ca_fn_{i}" for i, n in enumerate(nodes)}
    for node in nodes:
        if not isinstance(node, ast.GeneratorExp):
            continue
        body = [
            ast.copy_location(
                ast.Expr(value=ast.copy_location(ast.Yield(value=node.elt), node)), node
            )
        ]
        for index in reversed(range(len(node.generators))):
            gen = node.generators[index]
            for predicate in reversed(gen.ifs):
                body = [ast.copy_location(ast.If(test=predicate, body=body, orelse=[]), predicate)]
            iterable = (
                ast.copy_location(ast.Name(id=".0", ctx=ast.Load()), node)
                if index == 0
                else gen.iter
            )
            body = [
                ast.copy_location(
                    ast.For(target=gen.target, iter=iterable, body=body, orelse=[]), node
                )
            ]
        args = ast.arguments(
            posonlyargs=[],
            args=[ast.arg(arg=".0")],
            vararg=None,
            kwonlyargs=[],
            kw_defaults=[],
            kwarg=None,
            defaults=[],
        )
        synthetic = ast.copy_location(
            ast.FunctionDef(
                name="<genexpr>", args=args, body=body, decorator_list=[], type_params=[]
            ),
            node,
        )
        ast.fix_missing_locations(synthetic)
        generators[id(node)] = synthetic
        code_map[id(synthetic)] = code_map[id(node)]
        dynamic[id(synthetic)] = True
        functions[id(synthetic)] = functions[id(node)]
    # Lambda CIR uses the same function builder while preserving lexical code info.
    for node in nodes:
        if isinstance(node, ast.Lambda):
            node.name = "<lambda>"
            node.decorator_list = []
            node.type_params = []
    items = [
        NativeBuilder(
            tree, "ca_module", functions, codes=code_map, dynamic=dynamic, generators=generators
        ).fn
    ]
    for node in nodes:
        if isinstance(node, ast.Lambda):
            replacement = ast.FunctionDef(
                name="<lambda>",
                args=node.args,
                body=[ast.copy_location(ast.Return(value=node.body), node)],
                decorator_list=[],
                type_params=[],
            )
            ast.copy_location(replacement, node)
            ast.fix_missing_locations(replacement)
            code_map[id(replacement)] = code_map[id(node)]
            dynamic[id(replacement)] = True
        elif isinstance(node, ast.GeneratorExp):
            replacement = generators[id(node)]
        else:
            replacement = node
        items.append(
            NativeBuilder(
                replacement,
                functions[id(node)],
                functions,
                codes=code_map,
                dynamic=dynamic,
                generators=generators,
            ).fn
        )
    return Program(items)
