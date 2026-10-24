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

int ca_main(int argc, char **argv, const char *filename, const char *directory, const char *python_executable,
            int (*execute)(PyObject *)) {
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0;
    /* Resolve stdlib, extension paths and venv state from the selected Python,
       rather than guessing an installation from the generated binary's path. */
    PyStatus status = PyConfig_SetBytesString(&config, &config.program_name, python_executable);
    if (!PyStatus_Exception(status)) status = PyConfig_SetBytesArgv(&config, argc, argv);
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

PyObject *ca_unpack(PyObject *value, Py_ssize_t count) {
    PyObject *iterator = PyObject_GetIter(value);
    if (!iterator) return NULL;
    PyObject *result = PyTuple_New(count);
    if (!result) { Py_DECREF(iterator); return NULL; }
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *item = PyIter_Next(iterator);
        if (!item) {
            if (!PyErr_Occurred()) PyErr_Format(PyExc_ValueError,
                "not enough values to unpack (expected %zd, got %zd)", count, i);
            goto error;
        }
        PyTuple_SET_ITEM(result, i, item);
    }
    PyObject *extra = PyIter_Next(iterator);
    if (extra) {
        Py_DECREF(extra);
        PyErr_Format(PyExc_ValueError, "too many values to unpack (expected %zd)", count);
        goto error;
    }
    if (PyErr_Occurred()) goto error;
    Py_DECREF(iterator);
    return result;
error:;
    PyObject *error = PyErr_GetRaisedException();
    Py_DECREF(iterator); Py_DECREF(result);
    PyErr_SetRaisedException(error);
    return NULL;
}

PyObject *ca_join_string(Py_ssize_t count, PyObject **values) {
    PyObject *parts = PyTuple_New(count);
    if (!parts) return NULL;
    for (Py_ssize_t i = 0; i < count; i++) PyTuple_SET_ITEM(parts, i, Py_NewRef(values[i]));
    PyObject *separator = PyUnicode_FromString("");
    PyObject *result = separator ? PyUnicode_Join(separator, parts) : NULL;
    PyObject *error = result ? NULL : PyErr_GetRaisedException();
    Py_XDECREF(separator); Py_DECREF(parts);
    if (error) PyErr_SetRaisedException(error);
    return result;
}

int ca_delete_global(PyObject *globals, const char *name) {
    int status = PyDict_DelItemString(globals, name);
    if (status < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {
        PyErr_Clear();
        PyErr_Format(PyExc_NameError, "name '%s' is not defined", name);
    }
    return status;
}

static PyObject *ca_exception_instance(PyObject *value, int cause) {
    PyObject *instance;
    if (PyExceptionClass_Check(value)) instance = PyObject_CallNoArgs(value);
    else instance = Py_NewRef(value);
    if (!instance) return NULL;
    if (!PyExceptionInstance_Check(instance)) {
        Py_DECREF(instance);
        PyErr_SetString(PyExc_TypeError, cause ? "exception causes must derive from BaseException" :
            "exceptions must derive from BaseException");
        return NULL;
    }
    return instance;
}

int ca_raise(PyObject *exception, PyObject *cause) {
    if (!exception) {
        PyObject *handled = PyErr_GetHandledException();
        if (handled) PyErr_SetRaisedException(handled);
        else PyErr_SetString(PyExc_RuntimeError, "No active exception to reraise");
        return -1;
    }
    PyObject *instance = ca_exception_instance(exception, 0);
    if (!instance) return -1;
    if (cause) {
        PyObject *explicit = cause == Py_None ? NULL : ca_exception_instance(cause, 1);
        if (cause != Py_None && !explicit) { Py_DECREF(instance); return -1; }
        PyException_SetCause(instance, explicit);
    }
    /* SetObject also attaches the currently handled exception as context. */
    PyErr_SetObject((PyObject *)Py_TYPE(instance), instance);
    Py_DECREF(instance);
    return -1;
}

PyObject *ca_make_function(PyMethodDef *definition, PyObject *context, PyObject *module,
                          Py_ssize_t ndefaults, Py_ssize_t nkwdefaults,
                          PyObject **values, const char **names) {
    PyObject *defaults = PyTuple_New(ndefaults), *keywords = PyDict_New();
    PyObject *self = NULL, *function = NULL;
    if (!defaults || !keywords) goto done;
    for (Py_ssize_t i = 0; i < ndefaults; i++) PyTuple_SET_ITEM(defaults, i, Py_NewRef(values[i]));
    for (Py_ssize_t i = 0; i < nkwdefaults; i++)
        if (PyDict_SetItemString(keywords, names[i], values[ndefaults + i]) < 0) goto done;
    self = PyTuple_Pack(3, context, defaults, keywords);
    if (self) function = PyCFunction_NewEx(definition, self, module);
done:;
    PyObject *error = function ? NULL : PyErr_GetRaisedException();
    Py_XDECREF(defaults); Py_XDECREF(keywords); Py_XDECREF(self);
    if (error) PyErr_SetRaisedException(error);
    return function;
}

PyObject *ca_bind(const char *name, PyObject *args, PyObject *kwargs,
                  Py_ssize_t posonly, Py_ssize_t positional, Py_ssize_t kwonly,
                  int varargs, int varkw, const char **names,
                  PyObject *defaults, PyObject *kwdefaults) {
    Py_ssize_t supplied = PyTuple_GET_SIZE(args), ordinary = positional + kwonly;
    PyObject *bound = PyTuple_New(ordinary + varargs + varkw);
    PyObject *extra_keywords = varkw ? PyDict_New() : NULL;
    if (!bound || (varkw && !extra_keywords)) goto error;
    if (supplied > positional && !varargs) {
        PyErr_Format(PyExc_TypeError, "%s() takes at most %zd positional arguments (%zd given)",
                     name, positional, supplied);
        goto error;
    }
    for (Py_ssize_t i = 0; i < supplied && i < positional; i++)
        PyTuple_SET_ITEM(bound, i, Py_NewRef(PyTuple_GET_ITEM(args, i)));
    if (varargs) {
        PyObject *rest = PyTuple_GetSlice(args, positional, supplied);
        if (!rest) goto error;
        PyTuple_SET_ITEM(bound, ordinary, rest);
    }
    Py_ssize_t position = 0;
    PyObject *key, *value;
    while (kwargs && PyDict_Next(kwargs, &position, &key, &value)) {
        if (!PyUnicode_Check(key)) {
            PyErr_SetString(PyExc_TypeError, "keywords must be strings"); goto error;
        }
        Py_ssize_t index;
        for (index = posonly; index < ordinary; index++) {
            PyObject *parameter = PyUnicode_FromString(names[index]);
            if (!parameter) goto error;
            int equal = PyObject_RichCompareBool(key, parameter, Py_EQ);
            Py_DECREF(parameter);
            if (equal < 0) goto error;
            if (equal) break;
        }
        if (index == ordinary) {
            if (!varkw) {
                PyErr_Format(PyExc_TypeError, "%s() got an unexpected keyword argument '%U'", name, key);
                goto error;
            }
            if (PyDict_SetItem(extra_keywords, key, value) < 0) goto error;
        } else {
            if (PyTuple_GET_ITEM(bound, index)) {
                PyErr_Format(PyExc_TypeError, "%s() got multiple values for argument '%U'", name, key);
                goto error;
            }
            PyTuple_SET_ITEM(bound, index, Py_NewRef(value));
        }
    }
    Py_ssize_t required = positional - PyTuple_GET_SIZE(defaults);
    for (Py_ssize_t i = 0; i < ordinary; i++) {
        if (PyTuple_GET_ITEM(bound, i)) continue;
        PyObject *value = NULL;
        if (i < positional && i >= required) value = PyTuple_GET_ITEM(defaults, i - required);
        else if (i >= positional) value = PyDict_GetItemString(kwdefaults, names[i]);
        if (!value) {
            if (!PyErr_Occurred()) PyErr_Format(PyExc_TypeError,
                "%s() missing required %s argument: '%s'", name,
                i < positional ? "positional" : "keyword-only", names[i]);
            goto error;
        }
        PyTuple_SET_ITEM(bound, i, Py_NewRef(value));
    }
    if (varkw) {
        PyTuple_SET_ITEM(bound, ordinary + varargs, extra_keywords);
        extra_keywords = NULL;
    }
    return bound;
error:;
    PyObject *error = PyErr_GetRaisedException();
    Py_XDECREF(bound); Py_XDECREF(extra_keywords);
    if (error) PyErr_SetRaisedException(error);
    return NULL;
}

int ca_args_extend(PyObject *args, PyObject *values) {
    PyObject *result = PyObject_CallMethod(args, "extend", "(O)", values);
    if (!result) return -1;
    Py_DECREF(result);
    return 0;
}

int ca_keyword_set(PyObject *kwargs, const char *name, PyObject *value) {
    PyObject *key = PyUnicode_FromString(name);
    if (!key) return -1;
    int found = PyDict_Contains(kwargs, key);
    int status = -1;
    if (found == 1) PyErr_Format(PyExc_TypeError, "got multiple values for keyword argument '%U'", key);
    else if (found == 0) status = PyDict_SetItem(kwargs, key, value);
    Py_DECREF(key);
    return status;
}

int ca_keywords_merge(PyObject *kwargs, PyObject *values) {
    PyObject *keys = PyMapping_Keys(values);
    if (!keys) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
            PyErr_Format(PyExc_TypeError, "argument after ** must be a mapping, not %.200s", Py_TYPE(values)->tp_name);
        }
        return -1;
    }
    int status = -1;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(keys); i++) {
        PyObject *key = PyList_GET_ITEM(keys, i);
        int found = PyDict_Contains(kwargs, key);
        if (found < 0) goto done;
        if (found) {
            PyErr_Format(PyExc_TypeError, "got multiple values for keyword argument '%S'", key);
            goto done;
        }
        PyObject *value = PyObject_GetItem(values, key);
        if (!value) goto done;
        int inserted = PyDict_SetItem(kwargs, key, value);
        Py_DECREF(value);
        if (inserted < 0) goto done;
    }
    status = 0;
done:;
    PyObject *error = status < 0 ? PyErr_GetRaisedException() : NULL;
    Py_DECREF(keys);
    if (error) PyErr_SetRaisedException(error);
    return status;
}

PyObject *ca_call_ex(PyObject *function, PyObject *args, PyObject *kwargs) {
    PyObject *tuple = PyList_AsTuple(args);
    if (!tuple) return NULL;
    PyObject *result = PyObject_Call(function, tuple, kwargs);
    PyObject *error = result ? NULL : PyErr_GetRaisedException();
    Py_DECREF(tuple);
    if (error) PyErr_SetRaisedException(error);
    return result;
}

/* Exact integers only: subclasses and overflow retain Python's numeric dispatch.
   Conversion overflow is reported out-of-band and never truncates a Python int. */
#define CA_CHECKED_INTEGER(name, intrinsic, generic) \
PyObject *ca_checked_##name(PyObject *left, PyObject *right) { \
    if (PyLong_CheckExact(left) && PyLong_CheckExact(right)) { \
        int overflow_left = 0, overflow_right = 0; \
        long a = PyLong_AsLongAndOverflow(left, &overflow_left); \
        long b = PyLong_AsLongAndOverflow(right, &overflow_right); \
        long result; \
        if (PyErr_Occurred()) return NULL; \
        if (!overflow_left && !overflow_right && !intrinsic(a, b, &result)) \
            return PyLong_FromLong(result); \
    } \
    return generic(left, right); \
}
CA_CHECKED_INTEGER(add, __builtin_add_overflow, PyNumber_Add)
CA_CHECKED_INTEGER(subtract, __builtin_sub_overflow, PyNumber_Subtract)
CA_CHECKED_INTEGER(multiply, __builtin_mul_overflow, PyNumber_Multiply)

PyObject *ca_unpack_ex(PyObject *value, Py_ssize_t before, Py_ssize_t after) {
    PyObject *iterator = PyObject_GetIter(value);
    if (!iterator) return NULL;
    PyObject *result = PyTuple_New(before + after + 1), *rest = NULL;
    if (!result) goto error;
    for (Py_ssize_t i = 0; i < before; i++) {
        PyObject *item = PyIter_Next(iterator);
        if (!item) {
            if (!PyErr_Occurred()) PyErr_Format(PyExc_ValueError,
                "not enough values to unpack (expected at least %zd, got %zd)", before + after, i);
            goto error;
        }
        PyTuple_SET_ITEM(result, i, item);
    }
    rest = PySequence_List(iterator);
    if (!rest) goto error;
    Py_ssize_t size = PyList_GET_SIZE(rest);
    if (size < after) {
        PyErr_Format(PyExc_ValueError, "not enough values to unpack (expected at least %zd, got %zd)",
                     before + after, before + size);
        goto error;
    }
    for (Py_ssize_t i = 0; i < after; i++)
        PyTuple_SET_ITEM(result, before + 1 + i, Py_NewRef(PyList_GET_ITEM(rest, size - after + i)));
    if (PyList_SetSlice(rest, size - after, size, NULL) < 0) goto error;
    PyTuple_SET_ITEM(result, before, rest);
    Py_DECREF(iterator);
    return result;
error:;
    PyObject *error = PyErr_GetRaisedException();
    Py_XDECREF(rest); Py_XDECREF(result); Py_DECREF(iterator);
    if (error) PyErr_SetRaisedException(error);
    return NULL;
}
