#ifndef CALCIUM_RUNTIME_H
#define CALCIUM_RUNTIME_H
#define PY_SSIZE_T_CLEAN
#include <Python.h>
/* Runtime ABI 3: all inputs borrowed, object outputs owned; NULL/-1 = error. */
#define CALCIUM_RUNTIME_ABI 3
PyObject *ca_unicode_constant(const char *value, Py_ssize_t size, int intern);
PyObject *ca_lookup(PyObject *globals, const char *name);
PyObject *ca_call(PyObject *callable, Py_ssize_t npos, Py_ssize_t nkw,
                  PyObject **values, const char **names);
PyObject *ca_container(const char *kind, Py_ssize_t size, PyObject **values);
PyObject *ca_not(PyObject *value);
PyObject *ca_contains(PyObject *item, PyObject *container, int invert);
PyObject *ca_unpack(PyObject *value, Py_ssize_t count);
PyObject *ca_unpack_ex(PyObject *value, Py_ssize_t before, Py_ssize_t after);
PyObject *ca_join_string(Py_ssize_t count, PyObject **values);
int ca_delete_global(PyObject *globals, const char *name);
int ca_raise(PyObject *exception, PyObject *cause);
int ca_args_extend(PyObject *args, PyObject *values);
int ca_keyword_set(PyObject *kwargs, const char *name, PyObject *value);
int ca_keywords_merge(PyObject *kwargs, PyObject *values);
PyObject *ca_call_ex(PyObject *function, PyObject *args, PyObject *kwargs);
PyObject *ca_checked_add(PyObject *left, PyObject *right);
PyObject *ca_checked_subtract(PyObject *left, PyObject *right);
PyObject *ca_checked_multiply(PyObject *left, PyObject *right);
PyObject *ca_make_function(PyMethodDef *definition, PyObject *context, PyObject *module,
                          Py_ssize_t ndefaults, Py_ssize_t nkwdefaults,
                          PyObject **values, const char **names);
PyObject *ca_bind(const char *name, PyObject *args, PyObject *kwargs,
                  Py_ssize_t posonly, Py_ssize_t positional, Py_ssize_t kwonly,
                  int varargs, int varkw, const char **names,
                  PyObject *defaults, PyObject *kwdefaults);
int ca_set_text(PyObject *dict, const char *key, const char *text);
int ca_execute_source(const char *source, const char *filename, PyObject *globals);
int ca_main(int argc, char **argv, const char *filename, const char *directory, const char *python_executable,
            int (*execute)(PyObject *));

/* Project registry entries contain no interpreter-owned state. */
typedef struct {
    const char *name;
    const char *filename;
    const char *directory;
    int package;
    int namespace_package;
    const char **search_locations;
    int (*execute)(PyObject *);
} CaModule;
int ca_install_modules(const CaModule *modules, Py_ssize_t count);
int ca_run_module(const CaModule *entry, PyObject *globals, int module_entry);
PyObject *ca_import_name(PyObject *globals, const char *name, int level,
                        Py_ssize_t from_count, const char **from_names,
                        Py_ssize_t local_count, const char **local_names, PyObject **local_values);
PyObject *ca_import_from(PyObject *module, const char *name);
#endif
