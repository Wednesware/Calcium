#include "calcium_runtime.h"
#include <string.h>

typedef struct {
    PyObject_HEAD
    const CaModule *modules;
    Py_ssize_t count;
    PyObject *spec_factory;
} CaImporter;

static const CaModule *ca_find(CaImporter *self, const char *name) {
    for (Py_ssize_t i = 0; i < self->count; i++)
        if (!strcmp(self->modules[i].name, name)) return self->modules + i;
    return NULL;
}

static PyObject *ca_spec(const CaModule *entry, PyObject *loader) {
    PyObject *factory = Py_NewRef(((CaImporter *)loader)->spec_factory);
    PyObject *name = PyUnicode_FromString(entry->name);
    PyObject *args = name ? PyTuple_Pack(2, name, entry->namespace_package ? Py_None : loader) : NULL;
    Py_XDECREF(name);
    PyObject *kwargs = PyDict_New();
    PyObject *spec = NULL;
    if (!args || !kwargs) goto done;
    if (PyDict_SetItemString(kwargs, "is_package", entry->package ? Py_True : Py_False) < 0) goto done;
    spec = PyObject_Call(factory, args, kwargs);
    if (!spec) goto done;
    PyObject *filename = entry->namespace_package ? Py_NewRef(Py_None) : PyUnicode_DecodeFSDefault(entry->filename);
    if (!filename || PyObject_SetAttrString(spec, "origin", filename) < 0 ||
        PyObject_SetAttrString(spec, "has_location", entry->namespace_package ? Py_False : Py_True) < 0) {
        Py_XDECREF(filename); Py_CLEAR(spec); goto done;
    }
    Py_DECREF(filename);
    if (entry->package) {
        PyObject *path = PyList_New(0);
        if (!path) { Py_CLEAR(spec); goto done; }
        for (const char **p = entry->search_locations; p && *p; p++) {
            PyObject *directory = PyUnicode_DecodeFSDefault(*p);
            if (!directory || PyList_Append(path, directory) < 0) {
                Py_XDECREF(directory); Py_DECREF(path); Py_CLEAR(spec); goto done;
            }
            Py_DECREF(directory);
        }
        if (PyObject_SetAttrString(spec, "submodule_search_locations", path) < 0) Py_CLEAR(spec);
        Py_DECREF(path);
    }
done:;
    PyObject *error = spec ? NULL : PyErr_GetRaisedException();
    Py_DECREF(factory); Py_XDECREF(args); Py_XDECREF(kwargs);
    if (error) PyErr_SetRaisedException(error);
    return spec;
}

static PyObject *ca_finder_find_spec(PyObject *object, PyObject *args, PyObject *kwargs) {
    const char *name;
    PyObject *path = Py_None, *target = Py_None;
    static char *keywords[] = {"fullname", "path", "target", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|OO:find_spec", keywords, &name, &path, &target)) return NULL;
    (void)path; (void)target;
    const CaModule *entry = ca_find((CaImporter *)object, name);
    if (!entry) Py_RETURN_NONE;
    return ca_spec(entry, object);
}

static PyObject *ca_loader_create_module(PyObject *self, PyObject *spec) {
    (void)self; (void)spec;
    Py_RETURN_NONE;  /* importlib allocates and caches the module before exec. */
}

static PyObject *ca_loader_exec_module(PyObject *self, PyObject *module) {
    PyObject *name = PyObject_GetAttrString(module, "__name__");
    if (!name) return NULL;
    const char *text = PyUnicode_AsUTF8(name);
    const CaModule *entry = text ? ca_find((CaImporter *)self, text) : NULL;
    Py_DECREF(name);
    if (!entry) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_ImportError, "module not present in compiled project registry");
        return NULL;
    }
    PyObject *globals = PyModule_GetDict(module);
    if (!globals) return NULL;
    if (!PyDict_GetItemString(globals, "__builtins__") &&
        PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) < 0) return NULL;
    if (PyDict_SetItemString(globals, "__cached__", Py_None) < 0) return NULL;
    if (entry->execute && entry->execute(globals) < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *ca_loader_get_source(PyObject *self, PyObject *name) {
    (void)self; (void)name;
    Py_RETURN_NONE; /* No Python source is required by native modules. */
}

static PyObject *ca_loader_get_filename(PyObject *self, PyObject *name) {
    const char *text = PyUnicode_AsUTF8(name);
    if (!text) return NULL;
    const CaModule *entry = ca_find((CaImporter *)self, text);
    if (!entry) { PyErr_SetString(PyExc_ImportError, "unknown compiled module"); return NULL; }
    return PyUnicode_DecodeFSDefault(entry->filename);
}

static PyMethodDef ca_importer_methods[] = {
    {"find_spec", (PyCFunction)(void(*)(void))ca_finder_find_spec, METH_VARARGS | METH_KEYWORDS, NULL},
    {"create_module", ca_loader_create_module, METH_O, NULL},
    {"exec_module", ca_loader_exec_module, METH_O, NULL},
    {"get_source", ca_loader_get_source, METH_O, NULL},
    {"get_filename", ca_loader_get_filename, METH_O, NULL},
    {NULL, NULL, 0, NULL}
};

static int ca_importer_traverse(PyObject *self, visitproc visit, void *arg) {
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(((CaImporter *)self)->spec_factory);
    return 0;
}

static int ca_importer_clear(PyObject *self) {
    Py_CLEAR(((CaImporter *)self)->spec_factory);
    return 0;
}

static void ca_importer_dealloc(PyObject *self) {
    PyObject_GC_UnTrack(self);
    PyTypeObject *type = Py_TYPE(self);
    ca_importer_clear(self);
    PyObject_GC_Del(self);
    Py_DECREF(type);
}

int ca_install_modules(const CaModule *modules, Py_ssize_t count) {
    PyObject *finders = PySys_GetObject("meta_path");
    if (!finders || !PyList_Check(finders)) {
        PyErr_SetString(PyExc_RuntimeError, "sys.meta_path must be a list"); return -1;
    }
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(finders); i++) {
        PyObject *finder = PyList_GET_ITEM(finders, i);
        if (Py_TYPE(finder)->tp_dealloc == ca_importer_dealloc &&
            ((CaImporter *)finder)->modules == modules) return 0;
    }
    PyType_Slot slots[] = {{Py_tp_methods, ca_importer_methods}, {Py_tp_dealloc, ca_importer_dealloc}, {Py_tp_traverse, ca_importer_traverse}, {Py_tp_clear, ca_importer_clear}, {0, NULL}};
    PyType_Spec spec = {"calcium.CompiledImporter", sizeof(CaImporter), 0, Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, slots};
    PyObject *type = PyType_FromSpec(&spec);
    if (!type) return -1;
    CaImporter *finder = (CaImporter *)PyType_GenericAlloc((PyTypeObject *)type, 0);
    Py_DECREF(type);
    if (!finder) return -1;
    finder->modules = modules; finder->count = count;
    PyObject *util = PyImport_ImportModule("importlib.util");
    finder->spec_factory = util ? PyObject_GetAttrString(util, "spec_from_loader") : NULL;
    Py_XDECREF(util);
    if (!finder->spec_factory) { Py_DECREF(finder); return -1; }
    PyObject *machinery = PyImport_ImportModule("importlib.machinery");
    PyObject *path_finder = machinery ? PyObject_GetAttrString(machinery, "PathFinder") : NULL;
    Py_XDECREF(machinery);
    if (!path_finder) { Py_DECREF(finder); return -1; }
    Py_ssize_t index = PyList_GET_SIZE(finders);
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(finders); i++)
        if (PyList_GET_ITEM(finders, i) == path_finder) { index = i; break; }
    Py_DECREF(path_finder);
    int status = PyList_Insert(finders, index, (PyObject *)finder);
    Py_DECREF(finder);
    return status;
}

int ca_run_module(const CaModule *entry, PyObject *globals, int module_entry) {
    if (module_entry) {
        const char *dot = strrchr(entry->name, '.');
        PyObject *package = dot ? PyUnicode_FromStringAndSize(entry->name, dot - entry->name) : PyUnicode_FromString("");
        if (!package) return -1;
        if (dot) {
            const char *package_name = PyUnicode_AsUTF8(package);
            if (!package_name) { Py_DECREF(package); return -1; }
            PyObject *parent = PyImport_ImportModule(package_name);
            if (!parent) { Py_DECREF(package); return -1; }
            Py_DECREF(parent);
        }
        if (PyDict_SetItemString(globals, "__package__", package) < 0) { Py_DECREF(package); return -1; }
        Py_DECREF(package);
        PyObject *finders = PySys_GetObject("meta_path");
        PyObject *loader = NULL;
        for (Py_ssize_t i = 0; finders && i < PyList_GET_SIZE(finders); i++) {
            PyObject *candidate = PyList_GET_ITEM(finders, i);
            if (Py_TYPE(candidate)->tp_dealloc == ca_importer_dealloc && ca_find((CaImporter *)candidate, entry->name) == entry) {
                loader = candidate; break;
            }
        }
        if (!loader) { PyErr_SetString(PyExc_ImportError, "compiled entry loader is unavailable"); return -1; }
        PyObject *spec = ca_spec(entry, loader);
        if (!spec) return -1;
        int status = PyDict_SetItemString(globals, "__spec__", spec);
        if (!status) status = PyDict_SetItemString(globals, "__loader__", loader);
        Py_DECREF(spec);
        if (status < 0) return -1;
    }
    return entry->execute(globals);
}

PyObject *ca_import_name(PyObject *globals, const char *name, int level,
                        Py_ssize_t from_count, const char **from_names,
                        Py_ssize_t local_count, const char **local_names, PyObject **local_values) {
    PyObject *builtins = PyDict_GetItemString(globals, "__builtins__");
    if (builtins && PyModule_Check(builtins)) builtins = PyModule_GetDict(builtins);
    PyObject *importer = builtins && PyDict_Check(builtins) ? PyDict_GetItemString(builtins, "__import__") : NULL;
    if (!importer) { PyErr_SetString(PyExc_ImportError, "__import__ not found"); return NULL; }
    Py_INCREF(importer);
    PyObject *locals = local_count < 0 ? Py_NewRef(globals) : PyDict_New();
    PyObject *fromlist = from_count ? PyTuple_New(from_count) : Py_NewRef(Py_None);
    PyObject *module_name = PyUnicode_FromString(name), *relative = PyLong_FromLong(level);
    PyObject *result = NULL;
    if (!locals || !fromlist || !module_name || !relative) goto done;
    for (Py_ssize_t i = 0; i < local_count; i++)
        if (local_values[i] && PyDict_SetItemString(locals, local_names[i], local_values[i]) < 0) goto done;
    for (Py_ssize_t i = 0; i < from_count; i++) {
        PyObject *item = PyUnicode_FromString(from_names[i]);
        if (!item) goto done;
        PyTuple_SET_ITEM(fromlist, i, item);
    }
    result = PyObject_CallFunctionObjArgs(importer, module_name, globals, locals, fromlist, relative, NULL);
done:;
    PyObject *error = result ? NULL : PyErr_GetRaisedException();
    Py_DECREF(importer); Py_XDECREF(locals); Py_XDECREF(fromlist); Py_XDECREF(module_name); Py_XDECREF(relative);
    if (error) PyErr_SetRaisedException(error);
    return result;
}

PyObject *ca_import_from(PyObject *module, const char *name) {
    PyObject *value = PyObject_GetAttrString(module, name);
    if (value || !PyErr_ExceptionMatches(PyExc_AttributeError)) return value;
    PyErr_Clear();
    PyObject *module_name = PyObject_GetAttrString(module, "__name__");
    if (!module_name) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
            PyErr_Format(PyExc_ImportError, "cannot import name '%s' from an unnamed object", name);
        }
        return NULL;
    }
    PyObject *full_name = PyUnicode_FromFormat("%U.%s", module_name, name);
    if (!full_name) { Py_DECREF(module_name); return NULL; }
    value = PyDict_GetItemWithError(PyImport_GetModuleDict(), full_name);
    Py_DECREF(full_name);
    if (value) { Py_INCREF(value); Py_DECREF(module_name); return value; }
    if (!PyErr_Occurred()) PyErr_Format(PyExc_ImportError, "cannot import name '%s' from '%U'", name, module_name);
    Py_DECREF(module_name);
    return NULL;
}
