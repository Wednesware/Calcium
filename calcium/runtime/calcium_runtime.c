#include "calcium_runtime.h"
#include <string.h>

PyObject *ca_unicode_constant(const char *value, Py_ssize_t size, int intern) {
    PyObject *result = PyUnicode_DecodeUTF8(value, size, "surrogatepass");
    if (result && intern) PyUnicode_InternInPlace(&result);
    return result;
}

PyObject *ca_lookup(PyObject *globals, const char *name) {
    PyObject *value = PyDict_GetItemString(globals, name);
    if (value) return Py_NewRef(value);
    if (PyErr_Occurred()) return NULL;
    PyObject *builtins = PyDict_GetItemString(globals, "__builtins__");
    if (builtins && PyModule_Check(builtins)) builtins = PyModule_GetDict(builtins);
    if (builtins && PyDict_Check(builtins)) value = PyDict_GetItemString(builtins, name);
    if (value) return Py_NewRef(value);
    if (!PyErr_Occurred()) PyErr_Format(PyExc_NameError, "name '%s' is not defined", name);
    return NULL;
}

PyObject *ca_call(PyObject *callable, Py_ssize_t npos, Py_ssize_t nkw,
                  PyObject **values, const char **names) {
    PyObject *args = PyTuple_New(npos);
    if (!args) return NULL;
    for (Py_ssize_t i = 0; i < npos; i++) PyTuple_SET_ITEM(args, i, Py_NewRef(values[i]));
    PyObject *kwargs = nkw ? PyDict_New() : NULL;
    if (nkw && !kwargs) { Py_DECREF(args); return NULL; }
    for (Py_ssize_t i = 0; i < nkw; i++) {
        if (PyDict_SetItemString(kwargs, names[i], values[npos + i]) < 0) {
            Py_DECREF(args); Py_DECREF(kwargs); return NULL;
        }
    }
    PyObject *result = PyObject_Call(callable, args, kwargs);
    PyObject *error = result ? NULL : PyErr_GetRaisedException();
    Py_DECREF(args);
    Py_XDECREF(kwargs);
    if (error) PyErr_SetRaisedException(error);
    return result;
}

PyObject *ca_container(const char *kind, Py_ssize_t size, PyObject **values) {
    PyObject *result;
    if (!strcmp(kind, "list")) {
        result = PyList_New(size);
        if (result) for (Py_ssize_t i = 0; i < size; i++) PyList_SET_ITEM(result, i, Py_NewRef(values[i]));
        return result;
    }
    if (!strcmp(kind, "tuple")) {
        result = PyTuple_New(size);
        if (result) for (Py_ssize_t i = 0; i < size; i++) PyTuple_SET_ITEM(result, i, Py_NewRef(values[i]));
        return result;
    }
    int dict = !strcmp(kind, "dict");
    result = dict ? PyDict_New() : PySet_New(NULL);
    if (!result) return NULL;
    for (Py_ssize_t i = 0; i < size; i += dict ? 2 : 1) {
        int status = dict ? PyDict_SetItem(result, values[i], values[i + 1]) : PySet_Add(result, values[i]);
        if (status < 0) {
            PyObject *error = PyErr_GetRaisedException();
            Py_DECREF(result);
            PyErr_SetRaisedException(error);
            return NULL;
        }
    }
    return result;
}

PyObject *ca_not(PyObject *value) {
    int truth = PyObject_IsTrue(value);
    if (truth < 0) return NULL;
    return PyBool_FromLong(!truth);
}

PyObject *ca_contains(PyObject *item, PyObject *container, int invert) {
    int contained = PySequence_Contains(container, item);
    if (contained < 0) return NULL;
    return PyBool_FromLong(invert ? !contained : contained);
}

int ca_set_text(PyObject *dict, const char *key, const char *text) {
    PyObject *value = PyUnicode_DecodeUTF8(text, (Py_ssize_t)strlen(text), "surrogatepass");
    if (!value) return -1;
    int status = PyDict_SetItemString(dict, key, value);
    Py_DECREF(value);
    return status;
}

int ca_execute_source(const char *source, const char *filename, PyObject *globals) {
    PyObject *name = PyUnicode_DecodeFSDefault(filename);
    if (!name) return -1;
    PyCompilerFlags flags = {0};
    flags.cf_flags = PyCF_SOURCE_IS_UTF8 | PyCF_IGNORE_COOKIE;
    PyObject *code = Py_CompileStringObject(source, name, Py_file_input, &flags, 0);
    Py_DECREF(name);
    if (!code) return -1;
    PyObject *result = PyEval_EvalCode(code, globals, globals);
    PyObject *error = result ? NULL : PyErr_GetRaisedException();
    Py_DECREF(code);
    Py_XDECREF(result);
    if (error) PyErr_SetRaisedException(error);
    return error ? -1 : 0;
}

int ca_main(int argc, char **argv, const char *filename, const char *directory,
            int (*execute)(PyObject *)) {
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0;
    PyStatus status = PyConfig_SetBytesArgv(&config, argc, argv);
    if (!PyStatus_Exception(status)) status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        if (PyStatus_IsExit(status)) return status.exitcode;
        Py_ExitStatusException(status);
    }
    int exitcode = 0;
    PyObject *main_module = PyImport_AddModule("__main__");
    PyObject *globals = main_module ? PyModule_GetDict(main_module) : NULL;
    PyObject *path = PySys_GetObject("path");
    PyObject *entry = PyUnicode_DecodeFSDefault(directory);
    PyObject *args = PySys_GetObject("argv");
    PyObject *script = PyUnicode_DecodeFSDefault(filename);
    if (!globals || !entry || !script || !path || !args) goto error;
    if (PyList_Insert(path, 0, entry) < 0 || PyList_SetItem(args, 0, Py_NewRef(script)) < 0) goto error;
    if (PyDict_SetItemString(globals, "__file__", script) < 0 ||
        PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) < 0 ||
        PyDict_SetItemString(globals, "__package__", Py_None) < 0 ||
        PyDict_SetItemString(globals, "__spec__", Py_None) < 0) goto error;
    if (execute(globals) < 0) goto error;
    goto done;
error:
    /* PyErr_Print honors SystemExit, including its process status. */
    if (PyErr_Occurred()) PyErr_Print();
    exitcode = 1;
done:
    Py_XDECREF(entry);
    Py_XDECREF(script);
    if (Py_FinalizeEx() < 0) exitcode = 120;
    return exitcode;
}
