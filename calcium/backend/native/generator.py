"""C11 from verified CIR using CaRT values and explicit tracing roots."""

import json
import math

from ...ir.validator import validate
from ..generator_strings import cstr
from .kernels import emit_kernel


def generate(program, optimization=0, *, module_info=None):
    validate(program)
    constants = {}
    names = set()
    for fn in program.functions:
        for block in fn.blocks:
            for op in block.instructions:
                if op.op == "constant":
                    constants.setdefault(json.dumps(op.data, sort_keys=True), op.data)
                if op.op in {"load", "store", "native_raw_load"} and op.data not in fn.locals:
                    names.add(op.data)
    # Guards are real dynamic binding checks, never trusted annotations.
    for fn in program.functions:
        if fn.kernel:
            names.update(fn.kernel["guards"])
    if module_info:
        names.update({"__name__", "__package__", "__file__"})
        if module_info["is_package"]:
            names.add("__path__")
    slots = {name: i for i, name in enumerate(sorted(names))}
    pool = {key: i for i, key in enumerate(constants)}
    code = [
        '#include "cart.h"',
        "#include <float.h>",
        '_Static_assert(sizeof(long)==8 && CHAR_BIT==8 && DBL_MANT_DIG==53 && FLT_RADIX==2, "CaRT requires LP64 and IEEE binary64");',
        f"static CaValue globals[{max(1, len(slots))}];",
        f"static CaValue constants[{max(1, len(pool))}];",
    ]
    for fn in program.functions:
        code.append(f"static CaValue {fn.symbol}(CaRuntime *rt, CaValue *args);")
        if fn.native and fn.native.get("generator"):
            code.append(f"static int {fn.symbol}_resume(CaRuntime *,CaGenerator *,CaValue *);")
        if not fn.module:
            keys = ", ".join(cstr(n) for n in fn.parameters) or "NULL"
            code += [
                f"static const char *{fn.symbol}_keys[] = {{{keys}}};",
                f"static CaValue {fn.symbol}_defaults[{max(1, len(fn.parameters))}];",
                f"static const CaFunction {fn.symbol}_def = {{{cstr(fn.name)}, {len(fn.parameters)}, {fn.posonly}, {fn.symbol}_keys, {fn.symbol}, 0, {fn.positional}, {fn.symbol}_defaults, {fn.parameters.index(fn.vararg) if fn.vararg else -1}, {fn.parameters.index(fn.kwarg) if fn.kwarg else -1}}};",
            ]
    if module_info:
        code += [
            "static const char *global_names[]={" + ",".join(cstr(n) for n in slots) + "};",
            "static CaValue initialize(CaRuntime *rt);",
            f"CaModule {module_info['symbol']} = {{.name={cstr(module_info['name'])}, .package={cstr(module_info['package'])}, .is_package={int(module_info['is_package'])}, .names=global_names, .values=globals, .count={len(slots)}, .initialize=initialize}};",
        ]
    code.append(f"static CaFrame default_roots[{max(1, len(program.functions))}];")
    scalar = []
    for fn in program.functions:
        if fn.kernel and fn.kernel.get("enabled"):
            code.extend(emit_kernel(fn))
            scalar.append(fn.name)
    counts = {
        "generic_operations": 0,
        "native_calls": 0,
        "dynamic_calls": 0,
        "machine_numeric_heap_allocations": 0,
        "potential_heap_operations": 0,
        "guarded_direct_calls": 0,
        "large_frame_allocation_sites": 0,
        "scalar_argument_unpack_sites": sum(
            len(f.parameters) for f in program.functions if f.kernel and f.kernel.get("enabled")
        ),
        "scalar_return_pack_sites": len(scalar),
        "constant_heap_objects": sum(
            data["type"] == "str"
            or (data["type"] == "int" and not -(1 << 63) <= int(data["value"]) < (1 << 63))
            for data in constants.values()
        ),
    }
    for fn in program.functions:
        producers = {
            op.result: op for b in fn.blocks for op in b.instructions if op.result is not None
        }
        functions = {f.name: f for f in program.functions if not f.module}
        native = fn.native or {}
        generator = native.get("generator", False)
        freevars = native.get("freevars", [])
        cellvars = set(native.get("cellvars", []))
        local = {n: fn.registers + i for i, n in enumerate(fn.locals)}
        envslot = fn.registers + len(local)
        frame_count = envslot + bool(freevars)
        size = max(1, frame_count)
        heap_frame = size > 64
        counts["large_frame_allocation_sites"] += int(heap_frame)
        max_call_args = max(
            (
                len(op.args)
                for b in fn.blocks
                for op in b.instructions
                if op.op in {"call", "container", "native_class", "join_string", "function"}
            ),
            default=0,
        )
        if generator:
            code += [
                f"static CaValue {fn.symbol}(CaRuntime *rt,CaValue *args){{",
                f"CaValue value=ca_generator_new(rt,{fn.symbol}_resume,&{fn.symbol}_def,{frame_count});",
                "if(rt->error)return CA_UNBOUND;",
                "CaGenerator *g=(CaGenerator *)value.as.obj;CaValue *values=g->values;",
            ]
            for i, name in enumerate(fn.parameters):
                code.append(f"values[{local[name]}]=args[{i}];")
            if freevars:
                code.append(f"values[{envslot}]=rt->environment;")
            for name in sorted(cellvars):
                code.append(f"values[{local[name]}]=ca_cell(rt,values[{local[name]}]);")
            code += [
                f"g->coroutine={int(native.get('coroutine', False))};",
                "return rt->error?CA_UNBOUND:value;}",
                f"static int {fn.symbol}_resume(CaRuntime *rt,CaGenerator *g,CaValue *out){{",
                "CaValue *values=g->values;",
            ]
        else:
            code += [
                f"static CaValue {fn.symbol}(CaRuntime *rt,CaValue *args){{",
                f"CaValue *values=calloc({size},sizeof(CaValue));"
                if heap_frame
                else f"CaValue values[{size}]={{0}};",
            ]
        code += [
            "CaValue result=CA_UNBOUND;CaFrame frame;",
            'if(!values){ca_error(rt,"MemoryError","native frame allocation failed");return -1;}'
            if generator
            else 'if(!values){ca_error(rt,"MemoryError","native frame allocation failed");return CA_UNBOUND;}'
            if heap_frame
            else "",
        ]
        code.append(
            "if(!ca_enter(rt))return -1;"
            if generator
            else "if(!ca_enter(rt)){free(values);return CA_UNBOUND;}"
            if heap_frame
            else "if(!ca_enter(rt))return CA_UNBOUND;"
        )
        code += [
            f"CaValue call_values[{max(1, max_call_args)}];",
            f"ca_push(rt,&frame,values,{frame_count});",
        ]
        if not generator:
            for i, name in enumerate(fn.parameters):
                code.append(f"values[{local[name]}]=args[{i}];")
            if freevars:
                code.append(f"values[{envslot}]=rt->environment;")
            for name in sorted(cellvars):
                code += [
                    f"values[{local[name]}]=ca_cell(rt,values[{local[name]}]);",
                    "if(rt->error)goto error;",
                ]

        def cell(name):
            if name in cellvars:
                return f"values[{local[name]}]"
            if name in freevars:
                return f"ca_capture(values[{envslot}],{freevars.index(name)})"
            raise ValueError("unresolved native closure cell " + name)

        yield_ids = {
            op.result: i + 1
            for i, op in enumerate(
                op
                for block in fn.blocks
                for op in block.instructions
                if op.op in {"native_yield", "native_await"}
            )
        }
        if generator:
            code.append("switch(g->pc){case 0:break;")
            code += [f"case {index}:goto resume_{index};" for index in yield_ids.values()]
            code.append(
                'default:ca_error(rt,"RuntimeError","invalid generator resume state");goto error;}'
            )
        if fn.kernel and fn.kernel.get("enabled"):
            guards = []
            for name in fn.kernel["guards"]:
                index = slots[name]
                target = "&ca_builtin_range" if name == "range" else f"&{fn.symbol}_def"
                # Unbound range uses the immutable CaRT builtin; other names do not.
                guards.append(
                    f"(globals[{index}].tag==CA_FUNCTION_T && globals[{index}].as.fn=={target})"
                    + (f" || globals[{index}].tag==CA_UNBOUND_T" if name == "range" else "")
                )
            condition = " && ".join("(" + g + ")" for g in guards) or "1"
            inputs = ", ".join(f"values[{local[n]}].as.i" for n in fn.parameters) or "0"
            types = " && ".join(f"values[{local[n]}].tag==CA_INT_T" for n in fn.parameters) or "1"
            code += [
                f"if (({types}) && ({condition})) {{ long out=0; int status=k_{fn.symbol}(rt,(long[]){{{inputs}}},&out);",
                "if(status<0)goto error; if(status==1){result=ca_int(out);goto done;} }",
            ]
        code.append(f"goto {fn.blocks[0].name};")
        for block in fn.blocks:
            code += [f"{block.name}:;", "ca_safepoint(rt);"]
            for op in block.instructions:
                v = [f"values[{i}]" for i in op.args]
                dest = f"values[{op.result}]"
                expr = None
                if op.op == "constant":
                    expr = f"constants[{pool[json.dumps(op.data, sort_keys=True)]}]"
                elif op.op in {"load", "native_raw_load"}:
                    source = (
                        f"ca_cell_get({cell(op.data)})"
                        if op.data in cellvars or op.data in freevars
                        else f"values[{local[op.data]}]"
                        if op.data in local
                        else f"globals[{slots[op.data]}]"
                    )
                    expr = (
                        source
                        if op.op == "native_raw_load"
                        else f"ca_load(rt,{source},{cstr(op.data)},{int(op.data in local)})"
                    )
                elif op.op == "store":
                    if op.data in cellvars or op.data in freevars:
                        code.append(f"ca_cell_set({cell(op.data)},{v[0]});")
                    else:
                        source = (
                            f"values[{local[op.data]}]"
                            if op.data in local
                            else f"globals[{slots[op.data]}]"
                        )
                        code.append(f"{source}={v[0]};")
                elif op.op == "drop":
                    code.append(f"{v[0]}=CA_UNBOUND;")
                elif op.op == "clear":
                    code.append(f"values[{local[op.data]}]=CA_UNBOUND;")
                elif op.op in {"binary", "checked_binary", "inplace"}:
                    operator = {"add": "+", "subtract": "-", "multiply": "*"}.get(op.data)
                    expr = (
                        f"ca_binary_op(rt,{v[0]},{v[1]},{cstr(op.data)})"
                        if operator is None
                        else f"{'ca_inplace' if op.op == 'inplace' else 'ca_binary'}(rt,{v[0]},{v[1]},'{operator}')"
                    )
                    counts["generic_operations"] += 1
                    counts["potential_heap_operations"] += 1
                elif op.op == "unary":
                    expr = f"ca_unary(rt,{v[0]},{cstr(op.data)})"
                    counts["generic_operations"] += 1
                    counts["potential_heap_operations"] += 1
                elif op.op == "compare":
                    expr = f"ca_compare(rt,{v[0]},{v[1]},{cstr(op.data)})"
                    counts["generic_operations"] += 1
                    counts["potential_heap_operations"] += 1
                elif op.op in {"native_yield", "native_await"}:
                    index = yield_ids[op.result]
                    code += [
                        f"g->pc={index};*out={v[0]};ca_pop(rt,&frame);ca_leave(rt);return 1;",
                        f"resume_{index}:;",
                    ]
                    if op.op == "native_await":
                        code += [
                            "if(g->injected.tag!=CA_NONE_T && g->injected.tag!=CA_UNBOUND_T){CaValue failure=g->injected;g->injected=CA_NONE;ca_error_set(rt,failure);",
                            f"goto {'error' if op.error == 'error' else 'err_' + op.error};}}",
                        ]
                    expr = "g->sent"
                elif op.op == "native_class":
                    for index, value in enumerate(v):
                        code.append(f"call_values[{index}]={value};")
                    expr = f"ca_class(rt,{cstr(op.data)},{len(v)},call_values)"
                elif op.op == "native_class_load":
                    expr = f"ca_class_load(rt,{v[0]},{v[1]},{cstr(op.data)})"
                elif op.op == "error_get":
                    expr = "ca_error_get(rt)"
                elif op.op == "error_set":
                    code.append(f"ca_error_set(rt,{v[0]});")
                elif op.op == "exception_take":
                    expr = "ca_exception_take(rt)"
                elif op.op == "handled_get":
                    expr = "rt->handled.tag==CA_UNBOUND_T?CA_NONE:rt->handled"
                elif op.op == "handled_set":
                    code.append(f"rt->handled={v[0]};")
                elif op.op == "exception_raise":
                    code.append(f"ca_raise(rt,{v[0]},CA_NONE,0);")
                elif op.op == "exception_match":
                    expr = f"ca_bool(ca_exception_match(rt,{v[0]},{v[1]}))"
                elif op.op == "raise":
                    code.append(
                        f"ca_raise(rt,{v[0] if v else 'CA_UNBOUND'},{v[1] if len(v) > 1 else 'CA_NONE'},{int(len(v) > 1)});"
                    )
                elif op.op == "assert_fail":
                    code.append(
                        f"ca_raise(rt,ca_exception_new(rt,ca_type(CA_T_ASSERTIONERROR),{len(v)},{'&' + v[0] if v else 'NULL'}),CA_NONE,0);"
                    )
                elif op.op == "clear_binding":
                    if op.data in cellvars or op.data in freevars:
                        code.append(f"ca_cell_set({cell(op.data)},CA_UNBOUND);")
                    else:
                        code.append(
                            f"values[{local[op.data]}]=CA_UNBOUND;"
                            if op.data in local
                            else f"globals[{slots[op.data]}]=CA_UNBOUND;"
                        )
                elif op.op == "special":
                    expr = f"ca_special(rt,{v[0]},{cstr(op.data)})"
                elif op.op == "with_exit":
                    expr = f"ca_with_exit(rt,{v[0]},{v[1]})"
                elif op.op == "convert_string":
                    expr = f"ca_stringify(rt,{v[0]},{ {115: 0, 114: 1, 97: 2}[op.data] })"
                elif op.op == "format":
                    expr = f"ca_format(rt,{v[0]},{v[1]})"
                elif op.op == "join_string":
                    for index, value in enumerate(v):
                        code.append(f"call_values[{index}]={value};")
                    expr = f"ca_join_strings(rt,{len(v)},call_values)"
                elif op.op == "container":
                    for index, value in enumerate(v):
                        code.append(f"call_values[{index}]={value};")
                    expr = f"ca_container(rt,CA_{op.data.upper()}_T,{len(v)},call_values)"
                elif op.op == "call_ex":
                    expr = f"ca_call_ex(rt,{v[0]},{v[1]},{v[2]})"
                elif op.op == "to_tuple":
                    expr = f"ca_collection_builtin(rt,5,1,0,&{v[0]})"
                elif op.op in {"args_extend", "set_update"}:
                    code.append(f"ca_extend(rt,{v[0]},{v[1]});")
                elif op.op in {"kwargs_merge", "dict_merge"}:
                    code.append(
                        f"ca_kwargs_merge(rt,{v[0]},{v[1]},{int(op.op == 'kwargs_merge')});"
                    )
                elif op.op == "kwargs_set":
                    code.append(f"ca_kwargs_set(rt,{v[0]},{cstr(op.data)},{v[1]});")
                elif op.op == "unpack":
                    expr = f"ca_unpack(rt,{v[0]},{op.data})"
                elif op.op == "tuple_item":
                    expr = f"ca_getitem(rt,{v[0]},ca_int({op.data}))"
                elif op.op == "getitem":
                    expr = f"ca_getitem(rt,{v[0]},{v[1]})"
                elif op.op == "setitem":
                    code.append(f"ca_setitem(rt,{v[0]},{v[1]},{v[2]});")
                elif op.op == "slice":
                    expr = f"ca_slice(rt,{v[0]},{v[1]},{v[2]})"
                elif op.op in {"args_append", "set_add"}:
                    code.append(f"ca_append(rt,{v[0]},{v[1]});")
                elif op.op == "attribute":
                    expr = f"ca_attribute(rt,{v[0]},{cstr(op.data)},0)"
                elif op.op == "setattr":
                    code.append(f"ca_set_attribute(rt,{v[0]},{cstr(op.data)},{v[1]});")
                elif op.op == "import":
                    data = op.data
                    keys = ",".join(cstr(n) for n in data["fromlist"]) or "NULL"
                    code.append(f"static const char *imports_{op.result}[]={{{keys}}};")
                    context = "&" + module_info["symbol"] if module_info else "NULL"
                    expr = f"ca_import(rt,{context},{cstr(data['name'])},{data['level']},{len(data['fromlist'])},imports_{op.result})"
                elif op.op == "import_from":
                    expr = f"ca_attribute(rt,{v[0]},{cstr(op.data)},1)"
                elif op.op == "function":
                    target = next(f for f in program.functions if f.symbol == op.data["symbol"])
                    defaults = ["CA_UNBOUND"] * len(target.parameters)
                    for index, value in enumerate(v[: op.data["posdefaults"]]):
                        defaults[target.positional - op.data["posdefaults"] + index] = value
                    for name, value in zip(op.data["kwdefaults"], v[op.data["posdefaults"] :]):
                        defaults[target.parameters.index(name)] = value
                    if target.native and target.native.get("dynamic"):
                        captures = [
                            cell(n)
                            for n in op.data.get("captures", target.native.get("freevars", []))
                        ]
                        code += [
                            "{CaValue defaults[]={" + (",".join(defaults) or "CA_NONE") + "};",
                            "CaValue captures[]={" + (",".join(captures) or "CA_NONE") + "};",
                            f"{dest}=ca_closure(rt,&{target.symbol}_def,defaults,{len(captures)},captures);",
                            "}",
                        ]
                    else:
                        for index, value in enumerate(defaults):
                            code.append(f"{target.symbol}_defaults[{index}]={value};")
                        expr = f"ca_function(&{target.symbol}_def)"
                elif op.op == "call":
                    for index, value in enumerate(v[1:]):
                        code.append(f"call_values[{index}]={value};")
                    keys = ", ".join(cstr(n) for n in op.data["keywords"]) or "NULL"
                    code.append(f"static const char *keys_{op.result}[]={{{keys}}};")
                    expr = f"ca_call(rt,{v[0]},{op.data['positional']},{len(op.data['keywords'])},call_values,keys_{op.result})"
                    producer = producers.get(op.args[0])
                    candidate = (
                        functions.get(producer.data) if producer and producer.op == "load" else None
                    )
                    if (
                        optimization >= 2
                        and candidate
                        and not op.data["keywords"]
                        and len(candidate.parameters) == op.data["positional"]
                        and candidate.positional == len(candidate.parameters)
                        and not candidate.vararg
                        and not candidate.kwarg
                    ):
                        guard = (
                            f"{v[0]}.tag==CA_FUNCTION_T && {v[0]}.as.fn==&{candidate.symbol}_def"
                        )
                        expr = f"(({guard}) ? {candidate.symbol}(rt,call_values) : {expr})"
                        counts["guarded_direct_calls"] += 1
                    counts["native_calls"] += 1
                    counts["dynamic_calls"] += 1
                elif op.op == "iter":
                    expr = f"ca_iter(rt,{v[0]})"
                    counts["potential_heap_operations"] += 1
                else:
                    raise ValueError(f"native backend does not implement CIR operation {op.op}")
                if expr is not None:
                    code.append(f"{dest}={expr};")
                if op.error:
                    code.append(
                        f"if(rt->error)goto {'error' if op.error == 'error' else 'err_' + op.error};"
                    )
            t = block.terminator
            if t.op == "return":
                code += [f"result=values[{t.args[0]}];", "goto done;"]
            elif t.op == "jump":
                code.append(f"goto {t.targets[0]};")
            elif t.op == "branch":
                code += [
                    f"int truth_{block.name}=ca_truth(rt,values[{t.args[0]}]);",
                    f"values[{t.args[0]}]=CA_UNBOUND;",
                    f"if(rt->error)goto {'error' if t.error == 'error' else 'err_' + t.error};",
                    f"if(truth_{block.name})goto {t.targets[0]};else goto {t.targets[1]};",
                ]
            elif t.op == "next":
                code += [
                    f"int next_{block.name}=ca_next(rt,values[{t.args[0]}],&values[{t.result}]);",
                    f"values[{t.args[0]}]=CA_UNBOUND;",
                    f"if(rt->error)goto {'error' if t.error == 'error' else 'err_' + t.error};",
                    f"if(next_{block.name})goto {t.targets[0]};else goto {t.targets[1]};",
                ]
            elif t.op == "error":
                code.append(f"goto {'error' if t.error == 'error' else 'err_' + t.error};")
            else:
                raise ValueError(f"native backend does not implement terminator {t.op}")
        for label, hidden in fn.exception_handlers.items():
            code.append(f"err_{label}:;")
            code.append(f"memset(values,0,{fn.registers}*sizeof(CaValue));")
            code += [f"values[{local[n]}]=CA_UNBOUND;" for n in hidden]
            code.append(f"goto {label};")
        if generator:
            code += [
                "error:result=CA_UNBOUND;",
                "done:g->result=result;g->closed=1;ca_pop(rt,&frame);ca_leave(rt);return rt->error?-1:0;",
                "}",
            ]
        else:
            code += [
                "error:result=CA_UNBOUND;",
                "done:ca_pop(rt,&frame);ca_leave(rt);"
                + ("free(values);" if heap_frame else "")
                + "return result;",
                "}",
            ]
    if module_info:
        code += [
            f"void {module_info['symbol']}_roots(CaRuntime *rt, CaFrame *g, CaFrame *c) {{",
            f"ca_push(rt,g,globals,{len(slots)}); ca_push(rt,c,constants,{len(pool)});",
            *[
                f"ca_push(rt,&default_roots[{index}],{fn.symbol}_defaults,{len(fn.parameters)});"
                for index, fn in enumerate(program.functions)
                if not fn.module
            ],
            "}",
            "static CaValue initialize(CaRuntime *rt) {",
        ]
        for name, value in (
            ("__name__", module_info["name"]),
            ("__package__", module_info["package"]),
            ("__file__", module_info["file"]),
        ):
            expression = (
                "CA_NONE"
                if name == "__package__" and module_info.get("script")
                else f"ca_string(rt,{cstr(value)},{len(value.encode('utf-8'))})"
            )
            code += [f"globals[{slots[name]}]={expression};", "if(rt->error)return CA_UNBOUND;"]
    else:
        code += [
            "int main(void) { CaRuntime runtime={0}; CaRuntime *rt=&runtime; CaFrame global_roots,constant_roots;",
            f"ca_push(rt,&global_roots,globals,{len(slots)});",
            f"ca_push(rt,&constant_roots,constants,{len(pool)});",
        ]

    if module_info and module_info["is_package"]:
        paths = module_info["paths"]
        items = ",".join(f"ca_string(rt,{cstr(p)},{len(p.encode('utf-8'))})" for p in paths)
        code += [
            f"globals[{slots['__path__']}]=ca_container(rt,CA_LIST_T,{len(paths)},(CaValue[]){{{items}}});",
            "if(rt->error)return CA_UNBOUND;",
        ]

    def literal(data):
        kind, value = data["type"], data["value"]
        if kind == "NoneType":
            return "CA_NONE"
        if kind == "bool":
            return f"ca_bool({int(value)})"
        if kind == "int":
            return f"ca_integer(rt,{cstr(value)})"
        if kind == "float":
            f = float(value)
            text = (
                "NAN"
                if math.isnan(f)
                else ("-INFINITY" if f < 0 else "INFINITY")
                if math.isinf(f)
                else f.hex()
            )
            return f"ca_float({text})"
        if kind == "str":
            return f"ca_string(rt,{cstr(value)},{len(value.encode('utf-8'))})"
        if kind == "bytes":
            data = bytes.fromhex(value)
            text = '"' + "".join("\\%03o" % b for b in data) + '"'
            return f"ca_bytes(rt,{text},{len(data)})"
        if kind == "tuple":
            items = ",".join(literal(v) for v in value) or "CA_NONE"
            return f"ca_container(rt,CA_TUPLE_T,{len(value)},(CaValue[]){{{items}}})"
        raise ValueError(f"native constant {kind} is not implemented")

    for key, data in constants.items():
        code += [
            f"constants[{pool[key]}]={literal(data)};",
            "if(rt->error)return CA_UNBOUND;" if module_info else "if(rt->error)goto finish;",
        ]
    if module_info:
        code += ["return ca_module(rt,NULL);", "}"]
    else:
        code += [
            "ca_module(rt,NULL);",
            "finish:;",
            'int status=0;if(rt->error){fprintf(stderr,"%s: %s\\n",rt->error,rt->message);status=1;}',
            "if(fflush(stdout)!=0)status=1;ca_destroy(rt);return status;",
            "}",
        ]
    report = {
        "backend": "native",
        "specialized_functions": scalar,
        "generic_functions": [
            f.name for f in program.functions if not (f.kernel and f.kernel.get("enabled"))
        ],
        "function_details": [
            {
                "name": f.name,
                "symbol": f.symbol,
                "specialized": bool(f.kernel and f.kernel.get("enabled")),
                "generic_slow_path": True,
            }
            for f in program.functions
        ],
        "counts": counts,
        "scalar_materialization": "tagged integer at public return; no heap boxing for machine integers",
        "measurement": "static CIR operation sites, not measured runtime counts; generic arithmetic may promote integers or allocate strings",
    }
    return "\n".join(code) + "\n", report
