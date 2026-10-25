"""Emit the existing proven pure-integer kernel IR as checked C scalars."""


def emit_kernel(fn):
    lines = [
        f"static int k_{fn.symbol}(CaRuntime *rt,long *input,long *output) {{",
        "unsigned long iterations=0;",
    ]
    slots = {name: f"k{i}" for i, name in enumerate(fn.kernel["locals"])}
    for slot in slots.values():
        lines.append(f"long {slot}=0;int {slot}_set=0;")
    for i, name in enumerate(fn.parameters):
        lines.append(f"{slots[name]}=input[{i}];{slots[name]}_set=1;")
    counter = 0
    loops = []

    def fresh():
        nonlocal counter
        counter += 1
        return f"t{counter}"

    def expr(item):
        op = item["op"]
        out = fresh()
        lines.append(f"long {out}=0;")
        if op == "constant":
            value = "LONG_MIN" if item["value"] == -(1 << 63) else str(item["value"]) + "L"
            lines.append(f"{out}={value};")
        elif op == "local":
            slot = slots[item["name"]]
            lines.append(f"if(!{slot}_set)return 0;{out}={slot};")
        elif op in {"add", "subtract", "multiply"}:
            a, b = expr(item["left"]), expr(item["right"])
            lines.append(f"if(!ca_long_{op}({a},{b},&{out}))return 0;")
        elif op in {"positive", "negative"}:
            a = expr(item["value"])
            lines.append(
                f"{out}={a};"
                if op == "positive"
                else f"if(!ca_long_subtract(0,{a},&{out}))return 0;"
            )
        elif op == "recurse":
            args = ", ".join(expr(a) for a in item["args"]) or "0"
            status = fresh()
            lines.append(
                f"if(!ca_enter(rt))return -1;int {status}=k_{fn.symbol}(rt,(long[]){{{args}}},&{out});ca_leave(rt);if({status}!=1)return {status};"
            )
        return out

    def condition(item):
        if item["op"] == "truth":
            return expr(item["value"])
        a, b = expr(item["left"]), expr(item["right"])
        op = {"Lt": "<", "LtE": "<=", "Gt": ">", "GtE": ">=", "Eq": "==", "NotEq": "!="}[
            item["comparison"]
        ]
        return f"({a}{op}{b})"

    def body(items):
        for item in items:
            op = item["op"]
            if op == "assign":
                value = expr(item["value"])
                for name in item["targets"]:
                    lines.append(f"{slots[name]}={value};{slots[name]}_set=1;")
            elif op == "return":
                value = expr(item["value"])
                lines.append(f"*output={value};return 1;")
            elif op == "if":
                lines.append(f"if({condition(item['condition'])}){{")
                body(item["body"])
                lines.append("}else{")
                body(item["else"])
                lines.append("}")
            elif op in {"for", "while"}:
                broken = fresh()
                lines.append(f"int {broken}=0;")
                if op == "for":
                    args = [expr(a) for a in item["range"]]
                    start, stop, step = (
                        ("0L", args[0], "1L")
                        if len(args) == 1
                        else (args[0], args[1], args[2] if len(args) == 3 else "1L")
                    )
                    current, remaining = fresh(), fresh()
                    lines.append(
                        f"long {current}={start};unsigned long {remaining}=0;if(!ca_range_count({start},{stop},{step},&{remaining}))return 0;while({remaining}--){{"
                    )
                    slot = slots[item["target"]]
                    lines.append(
                        f"{slot}={current};{slot}_set=1;if({remaining}&&!ca_long_add({current},{step},&{current}))return 0;"
                    )
                else:
                    lines.append("while(1){")
                    test = condition(item["condition"])
                    lines.append(f"if(!({test}))break;")
                lines.append("if(++iterations>100000000UL)return 0;")
                loops.append(broken)
                body(item["body"])
                loops.pop()
                lines.append("}")
                lines.append(f"if(!{broken}){{")
                body(item["else"])
                lines.append("}")
            elif op == "break":
                lines.append(f"{loops[-1]}=1;break;")
            elif op == "continue":
                lines.append("continue;")

    body(fn.kernel["body"])
    lines += ["return 0;", "}"]
    return lines
