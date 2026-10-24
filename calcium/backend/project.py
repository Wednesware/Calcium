"""Link one native translation unit per source module into a project registry."""

import re
from hashlib import sha256

from .generator import cstr


def module_id(name):
    digest = sha256(name.encode("utf-8")).hexdigest()[:16]
    readable = re.sub(r"[^A-Za-z_0-9]", "_", name)[:60]
    return f"{readable}_{digest}"


def launcher(
    modules, entry_name, *, module_entry, directory, python_executable, extension_name, roots=()
):
    lines = [
        "/* Calcium compiled project registry; importlib controls initialization. */",
        '#include "program.h"',
    ]
    lines.append(
        "static const char *ca_roots[] = {"
        + ", ".join([*(cstr(str(r)) for r in roots), "NULL"])
        + "};"
    )
    for module in modules:
        if not module["namespace"]:
            lines.append(f"int {module['executor']}(PyObject *globals);")
    for i, module in enumerate(modules):
        paths = module.get("search_locations", [module["directory"]] if module["package"] else [])
        lines.append(
            f"static const char *ca_paths_{i}[] = {{"
            + ", ".join([*(cstr(p) for p in paths), "NULL"])
            + "};"
        )
    lines.append("static const CaModule ca_project_modules[] = {")
    for i, module in enumerate(modules):
        lines.append(
            "    {"
            + ", ".join(
                [
                    cstr(module["name"]),
                    cstr(module["source"]),
                    cstr(module["directory"]),
                    str(int(module["package"])),
                    str(int(module["namespace"])),
                    f"ca_paths_{i}",
                    module["executor"] or "NULL",
                ]
            )
            + "},"
        )
    lines.append("};")
    index = next(i for i, m in enumerate(modules) if m["name"] == entry_name)
    entry = modules[index]
    lines += [
        "static int ca_project_install(void) {",
        "    return ca_install_modules(ca_project_modules, sizeof(ca_project_modules) / sizeof(ca_project_modules[0]));",
        "}",
        "static int ca_project_execute(PyObject *g) {",
        "    if (ca_project_install() < 0) return -1;",
    ]
    if entry["package"]:
        lines += [
            '    PyErr_SetString(PyExc_ImportError, "package has no executable __main__.py; select --module package.entry or build an extension");',
            "    return -1;",
        ]
    else:
        lines.append(
            f"    return ca_run_module(&ca_project_modules[{index}], g, {int(module_entry)});"
        )
    lines += [
        "}",
        "#ifndef CALCIUM_EXTENSION",
        f"int main(int argc, char **argv) {{ ca_bundle_configure(ca_roots, {len(roots)}); return ca_main(argc, argv, {cstr(entry['source'])}, {cstr(directory)}, {cstr(python_executable)}, ca_project_execute); }}",
        "#else",
        "static int ca_exec_extension(PyObject *module) {",
        "    if (ca_project_install() < 0) return -1;",
        "    PyObject *g = PyModule_GetDict(module);",
        '    if (!PyDict_GetItemString(g, "__builtins__") && PyDict_SetItemString(g, "__builtins__", PyEval_GetBuiltins()) < 0) return -1;',
    ]
    if entry["package"]:
        lines += [
            f'    PyObject *path = Py_BuildValue("[s]", {cstr(entry["directory"])});',
            "    if (!path) return -1;",
            '    int status = PyDict_SetItemString(g, "__path__", path);',
            '    PyObject *spec = PyDict_GetItemString(g, "__spec__");',
            '    if (!status && spec && spec != Py_None) status = PyObject_SetAttrString(spec, "submodule_search_locations", path);',
            "    Py_DECREF(path);",
            f'    if (status < 0 || ca_set_text(g, "__package__", {cstr(entry_name)}) < 0) return -1;',
        ]
    lines += [
        f"    return {entry['executor']}(g);",
        "}",
        "static PyModuleDef_Slot ca_slots[] = {{Py_mod_exec, ca_exec_extension}, {0, NULL}};",
        f"static struct PyModuleDef ca_def = {{PyModuleDef_HEAD_INIT, {cstr(extension_name)}, NULL, 0, NULL, ca_slots, NULL, NULL, NULL}};",
        f"PyMODINIT_FUNC PyInit_{extension_name}(void) {{ return PyModuleDef_Init(&ca_def); }}",
        "#endif",
        "",
    ]
    return "\n".join(lines)
