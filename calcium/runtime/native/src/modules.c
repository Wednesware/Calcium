#include "standard.h"
/* Registry lookup never executes Python or searches the host filesystem. */
static CaModule *find(CaRuntime *rt, const char *name) {
  for (size_t i = 0; i < rt->module_count; i++)
    if (!strcmp(rt->modules[i]->name, name))
      return rt->modules[i];
  return ca_standard_find(rt, name);
}
static CaValue *binding(CaModule *m, const char *name) {
  for (size_t i = 0; i < m->count; i++)
    if (!strcmp(m->names[i], name))
      return &m->values[i];
  for (CaBinding *b = m->extra; b; b = b->next)
    if (!strcmp(b->name, name))
      return &b->value;
  return NULL;
}
void ca_set_attribute(CaRuntime *rt, CaValue v, const char *name,
                      CaValue value) {
  if (v.tag != CA_MODULE_T) {
    ca_object_set(rt, v, name, value);
    return;
  }
  CaModule *m = v.as.module;
  CaValue *slot = binding(m, name);
  if (slot) {
    *slot = value;
    return;
  }
  CaBinding *b = calloc(1, sizeof *b);
  char *copy = malloc(strlen(name) + 1);
  if (!b || !copy) {
    free(b);
    free(copy);
    ca_error(rt, "MemoryError", "module namespace allocation failed");
    return;
  }
  strcpy(copy, name);
  b->name = copy;
  b->value = value;
  b->next = m->extra;
  m->extra = b;
}
static CaModule *load(CaRuntime *, const char *);
CaValue ca_module_start(CaRuntime *rt, CaModule *m) {
  if (m->state)
    return ca_module_value(m); /* partial namespace during cycles */
  m->state = 1;
  m->initialize(rt);
  if (rt->error) {
    m->state = 0;
    for (size_t i = 0; i < m->count; i++)
      m->values[i] = CA_UNBOUND;
    return CA_UNBOUND;
  }
  m->state = 2;
  return ca_module_value(m);
}
static CaModule *load(CaRuntime *rt, const char *name) {
  CaModule *m = find(rt, name);
  if (m && m->state)
    return m;
  /* Initialize the parent before the child, then recheck: __init__ may have
     imported this same child itself. Bind a child only after it succeeds. */
  const char *dot = strrchr(name, '.');
  CaModule *parent = NULL;
  if (dot) {
    size_t n = (size_t)(dot - name);
    char *prefix = malloc(n + 1);
    if (!prefix) {
      ca_error(rt, "MemoryError", "import name allocation failed");
      return NULL;
    }
    memcpy(prefix, name, n);
    prefix[n] = 0;
    parent = load(rt, prefix);
    free(prefix);
    if (!parent)
      return NULL;
    if (!parent->is_package) {
      ca_error(rt, "ModuleNotFoundError", "import parent is not a package");
      return NULL;
    }
  }
  if (!m) {
    char message[220];
    snprintf(message, sizeof message, "No native module named '%s'", name);
    ca_error(rt, "ModuleNotFoundError", message);
    return NULL;
  }
  if (m->state)
    return m;
  ca_module_start(rt, m);
  if (rt->error)
    return NULL;
  if (parent)
    ca_set_attribute(rt, ca_module_value(parent), dot + 1, ca_module_value(m));
  return rt->error ? NULL : m;
}
CaValue ca_attribute(CaRuntime *rt, CaValue value, const char *name,
                     int importing) {
  if (value.tag == CA_GENERATOR_T) {
    CaGenerator *g = (CaGenerator *)value.as.obj;
    if (!strcmp(name, "__name__"))
      return ca_string(rt, g->definition->name, strlen(g->definition->name));
    int method = !strcmp(name, "send")    ? N_GEN_SEND
                 : !strcmp(name, "close") ? N_GEN_CLOSE
                 : !strcmp(name, "throw") ? N_GEN_THROW
                                          : 0;
    if (method)
      return ca_native_method(rt, value, method);
    ca_error(rt, "AttributeError", name);
    return CA_UNBOUND;
  }
  if (value.tag == CA_FUNCTION_T && value.as.fn->builtin == 6 &&
      !strcmp(name, "fromkeys"))
    return ca_native_function(N_DICT_FROMKEYS);
  if (value.tag == CA_CLASS_T || value.tag == CA_INSTANCE_T ||
      value.tag == CA_EXCEPTION_T || value.tag == CA_FOREIGN_T ||
      value.tag == CA_TYPE_T)
    return ca_object_attribute(rt, value, name);
  if (value.tag != CA_MODULE_T)
    return ca_bound_method(rt, value, name);
  CaModule *m = value.as.module;
  CaValue *v = binding(m, name);
  if (v && v->tag != CA_UNBOUND_T)
    return *v;
  if (importing) {
    /* CPython IMPORT_FROM permits a cached child during a circular import. */
    size_t n = strlen(m->name) + strlen(name) + 2;
    char *child = malloc(n);
    if (!child) {
      ca_error(rt, "MemoryError", "import name allocation failed");
      return CA_UNBOUND;
    }
    snprintf(child, n, "%s.%s", m->name, name);
    CaModule *cached = find(rt, child);
    free(child);
    if (cached && cached->state)
      return ca_module_value(cached);
  }
  char message[220];
  snprintf(message, sizeof message, "%s '%s' has no initialized name '%s'%s",
           m->is_package ? "package" : "module", m->name, name,
           m->state == 1 ? " (partially initialized module; circular import)"
                         : "");
  ca_error(rt, importing ? "ImportError" : "AttributeError", message);
  return CA_UNBOUND;
}
CaValue ca_import(CaRuntime *rt, CaModule *context, const char *name, int level,
                  int n, const char **from) {
  const char *package = context ? context->package : "";
  if (level && context) {
    CaValue *value = binding(context, "__package__");
    if (value && value->tag != CA_UNBOUND_T && value->tag != CA_NONE_T) {
      if (value->tag != CA_STR_T) {
        ca_error(rt, "TypeError", "__package__ must be a string");
        return CA_UNBOUND;
      }
      CaString *s = (CaString *)value->as.obj;
      if (memchr(s->data, 0, s->length)) {
        ca_error(rt, "ValueError", "embedded null character in package name");
        return CA_UNBOUND;
      }
      package = s->data;
    }
  }
  size_t length = strlen(package), base = length;
  if (level) {
    if (!length) {
      ca_error(rt, "ImportError",
               "attempted relative import with no known parent package");
      return CA_UNBOUND;
    }
    for (int i = 1; i < level; i++) {
      while (base && package[base - 1] != '.')
        base--;
      if (!base) {
        ca_error(rt, "ImportError",
                 "attempted relative import beyond top-level package");
        return CA_UNBOUND;
      }
      base--;
    }
  } else
    base = 0;
  size_t capacity = base + strlen(name) + 2;
  char *absolute = malloc(capacity);
  if (!absolute) {
    ca_error(rt, "MemoryError", "import name allocation failed");
    return CA_UNBOUND;
  }
  memcpy(absolute, package, base);
  size_t at = base;
  if (base && *name)
    absolute[at++] = '.';
  strcpy(absolute + at, name);
  CaModule *m = load(rt, absolute);
  if (!m) {
    free(absolute);
    return CA_UNBOUND;
  }
  if (n) {
    for (int i = 0; i < n && !rt->error; i++) {
      CaValue *v = binding(m, from[i]);
      if (m->is_package && (!v || v->tag == CA_UNBOUND_T)) {
        size_t size = strlen(absolute) + strlen(from[i]) + 2;
        char *child = malloc(size);
        if (!child) {
          ca_error(rt, "MemoryError", "import name allocation failed");
          break;
        }
        snprintf(child, size, "%s.%s", absolute, from[i]);
        /* Missing from-import attributes become ImportError in IMPORT_FROM.
           Known child modules execute; their internal errors propagate. */
        if (find(rt, child))
          load(rt, child);
        free(child);
      }
    }
  } else {
    char *dot = strchr(absolute, '.');
    if (dot) {
      *dot = 0;
      m = find(rt, absolute);
    }
  }
  free(absolute);
  return rt->error ? CA_UNBOUND : ca_module_value(m);
}
void ca_modules_destroy(CaRuntime *rt) {
  for (size_t i = 0; i < rt->module_count; i++) {
    CaModule *m = rt->modules[i];
    while (m->extra) {
      CaBinding *next = m->extra->next;
      free(m->extra->name);
      free(m->extra);
      m->extra = next;
    }
    m->state = 0;
  }
}
