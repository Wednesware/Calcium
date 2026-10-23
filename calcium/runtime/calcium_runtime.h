#ifndef CALCIUM_RUNTIME_H
#define CALCIUM_RUNTIME_H
#define PY_SSIZE_T_CLEAN
#include <Python.h>
/* Runtime ABI 1: all inputs borrowed, object outputs owned; NULL/-1 = error. */
#define CALCIUM_RUNTIME_ABI 1
PyObject *ca_unicode_constant(const char *value, Py_ssize_t size, int intern);
PyObject *ca_lookup(PyObject *globals, const char *name);
PyObject *ca_call(PyObject *callable, Py_ssize_t npos, Py_ssize_t nkw,
                  PyObject **values, const char **names);
PyObject *ca_container(const char *kind, Py_ssize_t size, PyObject **values);
PyObject *ca_not(PyObject *value);
PyObject *ca_contains(PyObject *item, PyObject *container, int invert);
int ca_set_text(PyObject *dict, const char *key, const char *text);
int ca_execute_source(const char *source, const char *filename, PyObject *globals);
int ca_main(int argc, char **argv, const char *filename, const char *directory, const char *python_executable,
            int (*execute)(PyObject *));
#endif
