"""C11 from verified CIR using CaRT values and explicit tracing roots."""

import json
import math

from ...ir.validator import validate
from ..generator_strings import cstr
from .kernels import emit_kernel


def generate(program, optimization=0):
    validate(program)
    constants = {}
    names = set()
    for fn in program.functions:
        for block in fn.blocks:
            for op in block.instructions:
                if op.op == "constant":
                    constants.setdefault(json.dumps(op.data, sort_keys=True), op.data)
                if op.op in {"load", "store"} and op.data not in fn.locals:
                    names.add(op.data)
    # Guards are real dynamic binding checks, never trusted annotations.
    for fn in program.functions:
        if fn.kernel:
            names.update(fn.kernel["guards"])
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
        if not fn.module:
            keys = ", ".join(cstr(n) for n in fn.parameters) or "NULL"
            code += [
                f"static const char *{fn.symbol}_keys[] = {{{keys}}};",
                f"static const CaFunction {fn.symbol}_def = {{{cstr(fn.name)}, {len(fn.parameters)}, {fn.posonly}, {fn.symbol}_keys, {fn.symbol}, 0}};",
            ]
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
            len(f.parameters) for f in program.functions if f.name in scalar
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
        local = {n: fn.registers + i for i, n in enumerate(fn.locals)}
        size = max(1, fn.registers + len(local))
        heap_frame = size > 64
        counts["large_frame_allocation_sites"] += int(heap_frame)
        max_call_args = max(
            (len(op.args) - 1 for b in fn.blocks for op in b.instructions if op.op == "call"),
            default=0,
        )
        code += [
            f"static CaValue {fn.symbol}(CaRuntime *rt, CaValue *args) {{",
            f"CaValue *values=calloc({size},sizeof(CaValue));"
            if heap_frame
            else f"CaValue values[{size}] = {{0}};",
            "CaValue result=CA_UNBOUND; CaFrame frame;",
            (
                'if(!values){ca_error(rt,"MemoryError","native frame allocation failed");return CA_UNBOUND;}'
                if heap_frame
                else ""
            ),
            (
                "if(!ca_enter(rt)){free(values);return CA_UNBOUND;}"
                if heap_frame
                else "if(!ca_enter(rt))return CA_UNBOUND;"
            ),
            f"CaValue call_values[{max(1, max_call_args)}];",
            f"ca_push(rt, &frame, values, {fn.registers + len(local)});",
        ]
        for i, name in enumerate(fn.parameters):
            code.append(f"values[{local[name]}]=args[{i}];")
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
                elif op.op == "load":
                    source = (
                        f"values[{local[op.data]}]"
                        if op.data in local
                        else f"globals[{slots[op.data]}]"
                    )
                    expr = f"ca_load(rt,{source},{cstr(op.data)},{int(op.data in local)})"
                elif op.op == "store":
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
                    operator = {"add": "+", "subtract": "-", "multiply": "*"}[op.data]
                    expr = f"ca_binary(rt,{v[0]},{v[1]},'{operator}')"
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
                elif op.op == "function":
                    expr = f"ca_function(&{op.data['symbol']}_def)"
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
                    code.append("if(rt->error)goto error;")
            t = block.terminator
            if t.op == "return":
                code += [f"result=values[{t.args[0]}];", "goto done;"]
            elif t.op == "jump":
                code.append(f"goto {t.targets[0]};")
            elif t.op == "branch":
                code += [
                    f"int truth_{block.name}=ca_truth(rt,values[{t.args[0]}]);",
                    f"values[{t.args[0]}]=CA_UNBOUND;",
                    "if(rt->error)goto error;",
                    f"if(truth_{block.name})goto {t.targets[0]};else goto {t.targets[1]};",
                ]
            elif t.op == "next":
                code += [
                    f"int next_{block.name}=ca_next(rt,values[{t.args[0]}],&values[{t.result}]);",
                    f"values[{t.args[0]}]=CA_UNBOUND;",
                    "if(rt->error)goto error;",
                    f"if(next_{block.name})goto {t.targets[0]};else goto {t.targets[1]};",
                ]
            else:
                raise ValueError(f"native backend does not implement terminator {t.op}")
        code += [
            "error: result=CA_UNBOUND;",
            "done: ca_pop(rt,&frame);ca_leave(rt);"
            + ("free(values);" if heap_frame else "")
            + "return result;",
            "}",
        ]
    code += [
        "int main(void) { CaRuntime runtime={0}; CaRuntime *rt=&runtime; CaFrame global_roots,constant_roots;",
        f"ca_push(rt,&global_roots,globals,{len(slots)});",
        f"ca_push(rt,&constant_roots,constants,{len(pool)});",
    ]
    for key, data in constants.items():
        kind, value = data["type"], data["value"]
        if kind == "NoneType":
            expr = "CA_NONE"
        elif kind == "bool":
            expr = f"ca_bool({int(value)})"
        elif kind == "int":
            expr = f"ca_integer(rt,{cstr(value)})"
        elif kind == "float":
            f = float(value)
            literal = (
                "NAN"
                if math.isnan(f)
                else ("-INFINITY" if f < 0 else "INFINITY")
                if math.isinf(f)
                else f.hex()
            )
            expr = f"ca_float({literal})"
        elif kind == "str":
            expr = f"ca_string(rt,{cstr(value)},{len(value.encode('utf-8'))})"
        else:
            raise ValueError(f"native constant {kind} is not implemented")
        code += [f"constants[{pool[key]}]={expr};", "if(rt->error)goto finish;"]
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
        "generic_functions": [f.name for f in program.functions if f.name not in scalar],
        "counts": counts,
        "scalar_materialization": "tagged integer at public return; no heap boxing for machine integers",
        "measurement": "static CIR operation sites, not measured runtime counts; generic arithmetic may promote integers or allocate strings",
    }
    return "\n".join(code) + "\n", report
