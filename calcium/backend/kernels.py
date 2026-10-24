"""Emit checked C scalars from typed kernel IR, boxing only at the boundary."""

import json

from ..frontend.constants import encode
from .generator_strings import cstr


class KernelEmitter:
    def __init__(self, fn, constants):
        self.fn, self.constants = fn, constants
        self.slots = {name: "k" + str(i) for i, name in enumerate(fn.kernel["locals"])}
        self.lines, self.counter, self.loops = [], 0, []

    def put(self, text):
        self.lines.append("    " + text)

    def temporary(self, prefix="t"):
        name = prefix + str(self.counter)
        self.counter += 1
        return name

    def expression(self, item):
        op = item["op"]
        out = self.temporary()
        self.put(f"long {out} = 0; PyObject *{out}_origin = NULL;")
        if op == "constant":
            value = (
                "LONG_MIN"
                if item["value"] == -(1 << (self.fn.kernel["bits"] - 1))
                else str(item["value"]) + "L"
            )
            self.put(f"{out} = {value};")
            key = json.dumps(encode(item["value"]), sort_keys=True)
            if key in self.constants:
                index = self.constants[key][0]
                self.put(f"{out}_origin = PyTuple_GET_ITEM(context, {index});")
        elif op == "local":
            slot = self.slots[item["name"]]
            self.put(f"if (!{slot}_set) return 0;")
            self.put(f"{out} = {slot}; {out}_origin = {slot}_origin;")
        elif op in {"add", "subtract", "multiply"}:
            a, b = self.expression(item["left"]), self.expression(item["right"])
            self.put(f"if (!ca_long_{op}({a}, {b}, &{out})) return 0;")
        elif op in {"positive", "negative"}:
            value = self.expression(item["value"])
            if op == "positive":
                self.put(f"{out} = {value}; {out}_origin = {value}_origin;")
            else:
                self.put(f"if (!ca_long_subtract(0, {value}, &{out})) return 0;")
        elif op == "recurse":
            args = [self.expression(a) for a in item["args"]]
            values = "(long[]){" + (", ".join(args) or "0") + "}"
            origins = "(PyObject *[]){" + (", ".join(a + "_origin" for a in args) or "NULL") + "}"
            status = self.temporary("status")
            self.put('if (Py_EnterRecursiveCall(" in a Calcium native kernel")) return -1;')
            self.put(
                f"int {status} = ca_kernel_{self.fn.symbol}(context, {values}, {origins}, &{out}, &{out}_origin);"
            )
            self.put("Py_LeaveRecursiveCall();")
            self.put(f"if ({status} != 1) return {status};")
        return out

    def condition(self, item):
        if item["op"] == "truth":
            return self.expression(item["value"])
        a, b = self.expression(item["left"]), self.expression(item["right"])
        operator = {"Lt": "<", "LtE": "<=", "Gt": ">", "GtE": ">=", "Eq": "==", "NotEq": "!="}[
            item["comparison"]
        ]
        return f"({a} {operator} {b})"

    def statements(self, items):
        for item in items:
            op = item["op"]
            if op == "assign":
                value = self.expression(item["value"])
                for target in item["targets"]:
                    slot = self.slots[target]
                    self.put(f"{slot} = {value}; {slot}_origin = {value}_origin; {slot}_set = 1;")
            elif op == "return":
                value = self.expression(item["value"])
                self.put(f"*output = {value}; *origin = {value}_origin; return 1;")
            elif op == "if":
                self.put(f"if ({self.condition(item['condition'])}) {{")
                self.statements(item["body"])
                self.put("} else {")
                self.statements(item["else"])
                self.put("}")
            elif op in {"for", "while"}:
                broken = self.temporary("broken")
                self.put(f"int {broken} = 0;")
                if op == "for":
                    values = [self.expression(v) for v in item["range"]]
                    start, stop, step = (
                        ("0L", values[0], "1L")
                        if len(values) == 1
                        else (values[0], values[1], values[2] if len(values) == 3 else "1L")
                    )
                    current, remaining = self.temporary("current"), self.temporary("remaining")
                    self.put(f"long {current} = {start}; unsigned long {remaining} = 0;")
                    self.put(
                        f"if (!ca_range_count({start}, {stop}, {step}, &{remaining})) return 0;"
                    )
                    self.put(f"while ({remaining}--) {{")
                    slot = self.slots[item["target"]]
                    self.put(f"{slot} = {current}; {slot}_origin = NULL; {slot}_set = 1;")
                    self.put(
                        f"if ({remaining} && !ca_long_add({current}, {step}, &{current})) return 0;"
                    )
                else:
                    self.put("while (1) {")
                    self.put(f"if (!({self.condition(item['condition'])})) break;")
                self.put("if (++iterations > 100000000UL) return 0;")
                self.loops.append(broken)
                self.statements(item["body"])
                self.loops.pop()
                self.put("}")
                self.put(f"if (!{broken}) {{")
                self.statements(item["else"])
                self.put("}")
            elif op == "break":
                self.put(f"{self.loops[-1]} = 1; break;")
            elif op == "continue":
                self.put("continue;")

    def emit(self):
        self.lines = [
            f"static int ca_kernel_{self.fn.symbol}(PyObject *context, long *inputs, PyObject **origins, long *output, PyObject **origin) {{"
        ]
        self.put("unsigned long iterations = 0;")
        for name, slot in self.slots.items():
            self.put(f"long {slot} = 0; int {slot}_set = 0; PyObject *{slot}_origin = NULL;")
        for index, name in enumerate(self.fn.kernel["parameters"]):
            slot = self.slots[name]
            self.put(f"{slot} = inputs[{index}]; {slot}_origin = origins[{index}]; {slot}_set = 1;")
        self.statements(self.fn.kernel["body"])
        self.put("return 0;")
        self.lines.append("}")
        return self.lines


def entry(fn, slots):
    args = ", ".join(slots[name] for name in fn.kernel["parameters"]) or "NULL"
    code = [
        f"    PyObject *kernel_args[] = {{{args}}};",
        f"    long kernel_values[{max(1, len(fn.kernel['parameters']))}] = {{0}};",
        "    int kernel_ok = 1, kernel_overflow = 0;",
    ]
    for index in range(len(fn.kernel["parameters"])):
        code += [
            f"    if (!PyLong_CheckExact(kernel_args[{index}])) kernel_ok = 0;",
            f"    if (kernel_ok) {{ kernel_values[{index}] = PyLong_AsLongAndOverflow(kernel_args[{index}], &kernel_overflow); if (kernel_overflow) kernel_ok = 0; }}",
        ]
    for name in fn.kernel["guards"]:
        code += [
            f"    if (kernel_ok) {{ PyObject *guard = ca_lookup(g, {cstr(name)});",
            "        if (!guard) { if (PyErr_ExceptionMatches(PyExc_NameError)) { PyErr_Clear(); kernel_ok = 0; } else goto error; }",
        ]
        if name == "range":
            code.append(
                "        else { kernel_ok = guard == (PyObject *)&PyRange_Type; Py_DECREF(guard); }"
            )
        else:
            code.append(
                f"        else {{ kernel_ok = PyCFunction_Check(guard) && PyCFunction_GetFunction(guard) == (PyCFunction)(void(*)(void)){fn.symbol} && PyTuple_Check(PyCFunction_GetSelf(guard)) && PyTuple_GET_ITEM(PyCFunction_GetSelf(guard), 0) == context; Py_DECREF(guard); }}"
            )
        code.append("    }")
    code += [
        "    if (PyErr_Occurred()) goto error;",
        "    if (kernel_ok) {",
        "        long output = 0; PyObject *origin = NULL;",
        f"        int status = ca_kernel_{fn.symbol}(context, kernel_values, kernel_args, &output, &origin);",
        "        if (status < 0) goto error;",
        "        if (status == 1) { result = origin ? Py_NewRef(origin) : PyLong_FromLong(output); if (!result) goto error; goto cleanup; }",
        "    }",
    ]
    return code
