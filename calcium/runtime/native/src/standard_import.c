#define _GNU_SOURCE
#include "dependencies.h"
#include <unistd.h>
static CaModule *lookup(CaRuntime *rt, const char *name) {
  for (size_t i = 0; i < rt->module_count; i++)
    if (!strcmp(rt->modules[i]->name, name))
      return rt->modules[i];
  return ca_standard_find(rt, name);
}
static CaValue spec(CaRuntime *rt, CaModule *m, CaValue name) {
  CaValue roots[] = {name, ca_dep_record(rt, "ModuleSpec"), CA_UNBOUND};
  CaFrame f;
  ca_push(rt, &f, roots, 3);
  roots[2] = ca_dep_record(rt, "NativeModuleLoader");
  ca_dep_put(rt, roots[2], "_module", ca_module_value(m));
  ca_dep_put(rt, roots[2], "exec_module",
             ca_native_method(rt, roots[2], D_IMPORT_EXEC_MODULE));
  ca_dep_put(rt, roots[1], "name", name);
  ca_dep_put(rt, roots[1], "loader", roots[2]);
  ca_dep_put(rt, roots[1], "origin",
             m->origin ? ca_string(rt, m->origin, strlen(m->origin)) : CA_NONE);
  ca_dep_put(rt, roots[1], "submodule_search_locations",
             m->is_package ? ca_container(rt, CA_LIST_T, 0, NULL) : CA_NONE);
  CaValue result = roots[1];
  ca_pop(rt, &f);
  return rt->error ? CA_UNBOUND : result;
}
CaValue ca_import_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                       const char **keys) {
  if (id == D_IMPORT_INVALIDATE) {
    if (n || nkw)
      goto bad;
    return CA_NONE;
  }
  if (id == D_IMPORT_MODULE) {
    CaValue out[2], d[] = {CA_NONE, CA_NONE};
    const char *names[] = {"name", "package"};
    if (!ca_native_arguments(rt, n, nkw, a, keys, 2, 1, names, d, out))
      return CA_UNBOUND;
    const char *name = ca_native_text(rt, out[0], 0);
    if (!name)
      return CA_UNBOUND;
    char *absolute = NULL;
    if (*name == '.') {
      const char *package =
          out[1].tag == CA_NONE_T ? NULL : ca_native_text(rt, out[1], 0);
      if (!package)
        return ca_dep_error(rt, "TypeError",
                            "relative import requires package");
      size_t count = 0;
      while (name[count] == '.')
        count++;
      size_t length = strlen(package);
      for (size_t i = 1; i < count; i++) {
        while (length && package[length - 1] != '.')
          length--;
        if (!length)
          return ca_dep_error(rt, "ImportError",
                              "relative import beyond top-level package");
        length--;
      }
      if (asprintf(&absolute, "%.*s%s%s", (int)length, package,
                   name[count] ? "." : "", name + count) < 0)
        return ca_dep_error(rt, "MemoryError", "import name allocation failed");
      name = absolute;
    }
    CaModule *m = ca_import_registry(rt, name);
    free(absolute);
    return m ? ca_module_value(m) : CA_UNBOUND;
  }
  if (id == D_IMPORT_FIND_SPEC) {
    CaValue out[2], d[] = {CA_NONE, CA_NONE};
    const char *names[] = {"name", "package"};
    if (!ca_native_arguments(rt, n, nkw, a, keys, 2, 1, names, d, out))
      return CA_UNBOUND;
    const char *name = ca_native_text(rt, out[0], 0);
    if (!name)
      return CA_UNBOUND;
    if (*name == '.')
      return ca_dep_error(rt, "NotImplementedError",
                          "native find_spec requires an absolute module name");
    CaModule *m = lookup(rt, name);
    return m ? spec(rt, m, out[0]) : CA_NONE;
  }
  if (id == D_IMPORT_SPEC_FILE) {
    CaValue out[3], d[] = {CA_NONE, CA_NONE, CA_NONE};
    const char *names[] = {"name", "location", "submodule_search_locations"};
    if (!ca_native_arguments(rt, n, nkw, a, keys, 3, 2, names, d, out))
      return CA_UNBOUND;
    const char *name = ca_native_text(rt, out[0], 0),
               *file = ca_native_text(rt, out[1], 1);
    if (!name || !file)
      return CA_UNBOUND;
    CaModule *m = NULL;
    /* Paths refer to a compiled origin, even when the original source is
       removed. Runtime source parsing or filesystem execution is deliberately
       absent. */
    for (size_t i = 0; i < rt->module_count; i++) {
      CaModule *candidate = rt->modules[i];
      if (candidate->origin && !strcmp(candidate->origin, file)) {
        m = candidate;
        break;
      }
    }
    if (!m)
      return ca_dep_error(rt, "ImportError",
                          "source file is not compiled into this executable; "
                          "include the package at build time");
    return spec(rt, m, out[0]);
  }
  if (id == D_IMPORT_MODULE_SPEC) {
    if (n != 1 || nkw)
      goto bad;
    CaValue loader = ca_dep_get(a[0], "loader"),
            module = ca_dep_get(loader, "_module");
    if (module.tag != CA_MODULE_T)
      return ca_dep_error(rt, "TypeError", "expected a native module spec");
    if (module.as.module->state)
      return ca_dep_error(
          rt, "ImportError",
          "isolated re-execution of an initialized compiled module is not "
          "implemented; use import_module for cached imports");
    return module;
  }
  if (id == D_IMPORT_EXEC_MODULE) {
    if (n != 2 || nkw)
      goto bad;
    CaValue expected = ca_dep_get(a[0], "_module");
    if (expected.tag != CA_MODULE_T || a[1].tag != CA_MODULE_T ||
        expected.as.module != a[1].as.module)
      return ca_dep_error(rt, "ImportError",
                          "module does not match its compiled loader");
    ca_module_start(rt, a[1].as.module);
    return rt->error ? CA_UNBOUND : CA_NONE;
  }
bad:
  return ca_dep_error(rt, "TypeError", "invalid native importlib arguments");
}
