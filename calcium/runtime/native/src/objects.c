#include "standard.h"
/* Module namespaces and instance dictionaries share native, allocation-free
   name lookup. New keys allocate once; writes to existing attributes do not. */
static CaValue get(CaValue mapping, const char *name) {
  if (mapping.tag != CA_DICT_T)
    return CA_UNBOUND;
  CaContainer *c = (CaContainer *)mapping.as.obj;
  size_t n = strlen(name);
  for (size_t i = 0; i < c->length; i += 2) {
    CaValue k = c->items[i];
    if (k.tag == CA_STR_T) {
      CaString *s = (CaString *)k.as.obj;
      if (s->length == n && !memcmp(s->data, name, n))
        return c->items[i + 1];
    }
  }
  return CA_UNBOUND;
}
static void set(CaRuntime *rt, CaValue mapping, const char *name,
                CaValue value) {
  CaContainer *c = (CaContainer *)mapping.as.obj;
  size_t n = strlen(name);
  for (size_t i = 0; i < c->length; i += 2) {
    CaString *s = (CaString *)c->items[i].as.obj;
    if (s->length == n && !memcmp(s->data, name, n)) {
      c->items[i + 1] = value;
      return;
    }
  }
  CaValue key = ca_string(rt, name, n);
  if (!rt->error)
    ca_setitem(rt, mapping, key, value);
}
CaValue ca_cell(CaRuntime *rt, CaValue value) {
  CaCell *c = ca_allocate(rt, sizeof *c, CA_CELL_T);
  if (!c)
    return CA_UNBOUND;
  c->value = value;
  return (CaValue){.tag = CA_CELL_T, .as.obj = (CaObj *)c};
}
CaValue ca_cell_get(CaValue cell) {
  return cell.tag == CA_CELL_T ? ((CaCell *)cell.as.obj)->value : CA_UNBOUND;
}
void ca_cell_set(CaValue cell, CaValue value) {
  if (cell.tag == CA_CELL_T)
    ((CaCell *)cell.as.obj)->value = value;
}
CaValue ca_capture(CaValue environment, size_t i) {
  return environment.tag == CA_TUPLE_T &&
                 i < ((CaContainer *)environment.as.obj)->length
             ? ((CaContainer *)environment.as.obj)->items[i]
             : CA_UNBOUND;
}
CaValue ca_closure(CaRuntime *rt, const CaFunction *definition,
                   CaValue *defaults, size_t n, CaValue *cells) {
  CaClosure *c =
      ca_allocate(rt, sizeof *c + (size_t)definition->count * sizeof(CaValue),
                  CA_CLOSURE_T);
  if (!c)
    return CA_UNBOUND;
  c->fn = *definition;
  c->fn.defaults = c->defaults;
  if (definition->count)
    memcpy(c->defaults, defaults, (size_t)definition->count * sizeof(CaValue));
  c->environment = ca_container(rt, CA_TUPLE_T, n, cells);
  return rt->error ? CA_UNBOUND
                   : (CaValue){.tag = CA_CLOSURE_T, .as.obj = (CaObj *)c};
}
CaValue ca_class(CaRuntime *rt, const char *name, size_t n, CaValue *bases) {
  if (n > 1 || (n && bases[0].tag != CA_CLASS_T && bases[0].tag != CA_TYPE_T)) {
    ca_error(rt, "TypeError", "native class requires at most one class base");
    return CA_UNBOUND;
  }
  if (n && bases[0].tag == CA_TYPE_T && bases[0].as.i != CA_T_OBJECT &&
      !ca_subclass(rt, bases[0], ca_type(CA_T_BASEEXCEPTION))) {
    ca_error(rt, "NotImplementedError",
             "subclassing native scalar/library types is not implemented");
    return CA_UNBOUND;
  }
  CaClass *c = ca_allocate(rt, sizeof *c, CA_CLASS_T);
  if (!c)
    return CA_UNBOUND;
  c->name = ca_string(rt, name, strlen(name));
  c->namespace_ = ca_container(rt, CA_DICT_T, 0, NULL);
  c->parent = n ? bases[0] : ca_type(CA_T_OBJECT);
  c->fields = CA_NONE;
  if (c->parent.tag == CA_CLASS_T) {
    CaClass *p = (CaClass *)c->parent.as.obj;
    c->dataclass = p->dataclass;
    c->fields = p->fields;
  }
  return rt->error ? CA_UNBOUND
                   : (CaValue){.tag = CA_CLASS_T, .as.obj = (CaObj *)c};
}
static CaValue class_get(CaValue type, const char *name) {
  while (type.tag == CA_CLASS_T) {
    CaClass *c = (CaClass *)type.as.obj;
    CaValue v = get(c->namespace_, name);
    if (v.tag != CA_UNBOUND_T)
      return v;
    type = c->parent;
  }
  return CA_UNBOUND;
}
CaValue ca_namespace_get(CaValue mapping, const char *name) {
  return get(mapping, name);
}
void ca_namespace_set(CaRuntime *rt, CaValue mapping, const char *name,
                      CaValue value) {
  set(rt, mapping, name, value);
}
CaValue ca_class_lookup(CaValue type, const char *name) {
  return class_get(type, name);
}
CaValue ca_class_load(CaRuntime *rt, CaValue type, CaValue fallback,
                      const char *name) {
  CaValue v = get(((CaClass *)type.as.obj)->namespace_, name);
  return v.tag == CA_UNBOUND_T ? ca_load(rt, fallback, name, 0) : v;
}
CaValue ca_bound(CaRuntime *rt, CaValue receiver, CaValue function) {
  CaBound *b = ca_allocate(rt, sizeof *b, CA_BOUND_T);
  if (!b)
    return CA_UNBOUND;
  b->receiver = receiver;
  b->function = function;
  return (CaValue){.tag = CA_BOUND_T, .as.obj = (CaObj *)b};
}
static CaValue bind(CaRuntime *rt, CaValue attribute, CaValue instance,
                    CaValue type) {
  if (attribute.tag == CA_FOREIGN_T) {
    CaForeign *f = (CaForeign *)attribute.as.obj;
    if (f->kind == 1)
      return f->state; /* staticmethod */
    if (f->kind == 2)
      return ca_bound(rt, type, f->state); /* classmethod */
    if (f->kind == 3 && instance.tag != CA_NONE_T) {
      CaValue v = instance;
      return ca_call(rt, f->state, 1, 0, &v, NULL);
    } /* property */
  }
  if (instance.tag != CA_NONE_T &&
      (attribute.tag == CA_FUNCTION_T || attribute.tag == CA_CLOSURE_T))
    return ca_bound(rt, instance, attribute);
  return attribute;
}
CaValue ca_object_attribute(CaRuntime *rt, CaValue value, const char *name) {
  if (value.tag == CA_FOREIGN_T || value.tag == CA_TYPE_T)
    return ca_foreign_attribute(rt, value, name);
  if (value.tag == CA_CLASS_T) {
    CaClass *c = (CaClass *)value.as.obj;
    if (!strcmp(name, "__name__") || !strcmp(name, "__qualname__"))
      return c->name;
    if (!strcmp(name, "__dict__"))
      return c->namespace_;
    CaValue v = class_get(value, name);
    if (v.tag != CA_UNBOUND_T)
      return bind(rt, v, CA_NONE, value);
  } else if (value.tag == CA_INSTANCE_T) {
    CaInstance *i = (CaInstance *)value.as.obj;
    if (!strcmp(name, "__class__"))
      return i->type;
    if (!strcmp(name, "__dict__"))
      return i->namespace_;
    CaValue v = class_get(i->type, name);
    if (v.tag == CA_FOREIGN_T && ((CaForeign *)v.as.obj)->kind == 3)
      return bind(rt, v, value, i->type);
    CaValue own = get(i->namespace_, name);
    if (own.tag != CA_UNBOUND_T)
      return own;
    if (v.tag != CA_UNBOUND_T)
      return bind(rt, v, value, i->type);
    CaValue missing = class_get(i->type, "__getattr__");
    if (missing.tag != CA_UNBOUND_T) {
      CaValue args[] = {value, ca_string(rt, name, strlen(name))};
      return rt->error ? CA_UNBOUND : ca_call(rt, missing, 2, 0, args, NULL);
    }
  } else if (value.tag == CA_EXCEPTION_T) {
    CaException *e = (CaException *)value.as.obj;
    if (!strcmp(name, "__class__"))
      return e->type;
    if (!strcmp(name, "args")) {
      if (e->args.tag == CA_UNBOUND_T) {
        CaValue text = e->message;
        e->args = ca_container(rt, CA_TUPLE_T, 1, &text);
      }
      return e->args;
    }
    if (!strcmp(name, "__cause__"))
      return e->cause;
    if (!strcmp(name, "__context__"))
      return e->context;
    CaValue v = class_get(e->type, name);
    if (v.tag != CA_UNBOUND_T)
      return bind(rt, v, value, e->type);
  }
  ca_error(rt, "AttributeError", name);
  return CA_UNBOUND;
}
void ca_object_set(CaRuntime *rt, CaValue value, const char *name, CaValue v) {
  if (value.tag == CA_FOREIGN_T &&
      ((CaForeign *)value.as.obj)->kind == CA_F_HTTP_SERVER) {
    ca_http_set(rt, value, name, v);
    return;
  }
  if (value.tag == CA_CLASS_T) {
    set(rt, ((CaClass *)value.as.obj)->namespace_, name, v);
    return;
  }
  if (value.tag == CA_INSTANCE_T) {
    CaInstance *i = (CaInstance *)value.as.obj;
    CaValue descriptor = class_get(i->type, name);
    if (descriptor.tag == CA_FOREIGN_T &&
        ((CaForeign *)descriptor.as.obj)->kind == 3) {
      ca_error(rt, "AttributeError", "property has no setter");
      return;
    }
    if (!strcmp(name, "__class__")) {
      ca_error(rt, "TypeError", "dynamic __class__ assignment is unsupported");
      return;
    }
    set(rt, i->namespace_, name, v);
    return;
  }
  ca_error(rt, "AttributeError",
           "object does not support attribute assignment");
}
static int parent(int kind) {
  switch (kind) {
  case CA_T_EXCEPTION:
  case CA_T_SYSTEMEXIT:
  case CA_T_KEYBOARDINTERRUPT:
    return CA_T_BASEEXCEPTION;
  case CA_T_ARITHMETICERROR:
    return CA_T_EXCEPTION;
  case CA_T_FLOATINGPOINTERROR:
    return CA_T_ARITHMETICERROR;
  case CA_T_SYNTAXERROR:
    return CA_T_EXCEPTION;
  case CA_T_INDENTATIONERROR:
    return CA_T_SYNTAXERROR;
  case CA_T_TABERROR:
    return CA_T_INDENTATIONERROR;
  case CA_T_CONNECTIONERROR:
    return CA_T_OSERROR;
  case CA_T_BROKENPIPEERROR:
    return CA_T_CONNECTIONERROR;
  case CA_T_CONNECTIONABORTEDERROR:
    return CA_T_CONNECTIONERROR;
  case CA_T_CONNECTIONREFUSEDERROR:
    return CA_T_CONNECTIONERROR;
  case CA_T_CONNECTIONRESETERROR:
    return CA_T_CONNECTIONERROR;
  case CA_T_EOFERROR:
    return CA_T_EXCEPTION;
  case CA_T_BUFFERERROR:
    return CA_T_EXCEPTION;
  case CA_T_GENERATOREXIT:
    return CA_T_BASEEXCEPTION;
  case CA_T_STOPASYNCITERATION:
    return CA_T_EXCEPTION;
  case CA_T_BLOCKINGIOERROR:
    return CA_T_OSERROR;
  case CA_T_CHILDPROCESSERROR:
    return CA_T_OSERROR;
  case CA_T_INTERRUPTEDERROR:
    return CA_T_OSERROR;
  case CA_T_ISADIRECTORYERROR:
    return CA_T_OSERROR;
  case CA_T_NOTADIRECTORYERROR:
    return CA_T_OSERROR;
  case CA_T_PROCESSLOOKUPERROR:
    return CA_T_OSERROR;
  case CA_T_UNICODETRANSLATEERROR:
    return CA_T_UNICODEERROR;
  case CA_T_SYSTEMERROR:
    return CA_T_EXCEPTION;
  case CA_T_REFERENCEERROR:
    return CA_T_EXCEPTION;
  case CA_T_PYTHONFINALIZATIONERROR:
    return CA_T_RUNTIMEERROR;
  case CA_T_WARNING:
    return CA_T_EXCEPTION;
  case CA_T_USERWARNING:
    return CA_T_WARNING;
  case CA_T_DEPRECATIONWARNING:
    return CA_T_WARNING;
  case CA_T_PENDINGDEPRECATIONWARNING:
    return CA_T_WARNING;
  case CA_T_SYNTAXWARNING:
    return CA_T_WARNING;
  case CA_T_RUNTIMEWARNING:
    return CA_T_WARNING;
  case CA_T_FUTUREWARNING:
    return CA_T_WARNING;
  case CA_T_IMPORTWARNING:
    return CA_T_WARNING;
  case CA_T_UNICODEWARNING:
    return CA_T_WARNING;
  case CA_T_BYTESWARNING:
    return CA_T_WARNING;
  case CA_T_RESOURCEWARNING:
    return CA_T_WARNING;
  case CA_T_ENCODINGWARNING:
    return CA_T_WARNING;
  case CA_T_ZERODIVISIONERROR:
  case CA_T_OVERFLOWERROR:
    return CA_T_ARITHMETICERROR;
  case CA_T_UNBOUNDLOCALERROR:
    return CA_T_NAMEERROR;
  case CA_T_KEYERROR:
  case CA_T_INDEXERROR:
    return CA_T_LOOKUPERROR;
  case CA_T_FILENOTFOUNDERROR:
  case CA_T_FILEEXISTSERROR:
  case CA_T_PERMISSIONERROR:
    return CA_T_OSERROR;
  case CA_T_MODULENOTFOUNDERROR:
    return CA_T_IMPORTERROR;
  case CA_T_RECURSIONERROR:
  case CA_T_NOTIMPLEMENTEDERROR:
    return CA_T_RUNTIMEERROR;
  case CA_T_CANCELLEDERROR:
    return CA_T_BASEEXCEPTION;
  case CA_T_TIMEOUTERROR:
    return CA_T_OSERROR;
  case CA_T_UNICODEENCODEERROR:
  case CA_T_UNICODEDECODEERROR:
    return CA_T_UNICODEERROR;
  case CA_T_UNICODEERROR:
    return CA_T_VALUEERROR;
  case CA_T_JSONDECODEERROR:
    return CA_T_VALUEERROR;
  case CA_T_SQL_ERROR:
    return CA_T_EXCEPTION;
  case CA_T_SQL_DATABASEERROR:
  case CA_T_SQL_INTERFACEERROR:
    return CA_T_SQL_ERROR;
  case CA_T_SQL_OPERATIONALERROR:
  case CA_T_SQL_INTEGRITYERROR:
  case CA_T_SQL_PROGRAMMINGERROR:
  case CA_T_SQL_DATAERROR:
    return CA_T_SQL_DATABASEERROR;
  case CA_T_BOOL:
    return CA_T_INT;
  default:
    return kind > CA_T_BASEEXCEPTION && kind < 300 ? CA_T_EXCEPTION
                                                   : CA_T_OBJECT;
  }
}
int ca_subclass(CaRuntime *rt, CaValue sub, CaValue type) {
  if (sub.tag == CA_FUNCTION_T && sub.as.fn->builtin == 2)
    sub = ca_type(CA_T_RANGE);
  if (type.tag == CA_FUNCTION_T && type.as.fn->builtin == 2)
    type = ca_type(CA_T_RANGE);
  if (sub.tag == CA_FUNCTION_T && sub.as.fn->builtin >= 4 &&
      sub.as.fn->builtin <= 7)
    sub = ca_type(CA_T_LIST + sub.as.fn->builtin - 4);
  if (type.tag == CA_FUNCTION_T && type.as.fn->builtin >= 4 &&
      type.as.fn->builtin <= 7)
    type = ca_type(CA_T_LIST + type.as.fn->builtin - 4);
  if (type.tag == CA_TUPLE_T) {
    CaContainer *c = (CaContainer *)type.as.obj;
    for (size_t i = 0; i < c->length; i++)
      if (ca_subclass(rt, sub, c->items[i]))
        return 1;
    return 0;
  }
  if (type.tag != CA_CLASS_T && type.tag != CA_TYPE_T) {
    ca_error(rt, "TypeError", "issubclass expects a class");
    return 0;
  }
  if (sub.tag != CA_CLASS_T && sub.tag != CA_TYPE_T) {
    ca_error(rt, "TypeError", "issubclass expects a class");
    return 0;
  }
  while (sub.tag == CA_CLASS_T) {
    if (type.tag == CA_CLASS_T && sub.as.obj == type.as.obj)
      return 1;
    sub = ((CaClass *)sub.as.obj)->parent;
  }
  if (type.tag == CA_CLASS_T)
    return 0;
  int id = (int)sub.as.i;
  for (;;) {
    if (id == type.as.i)
      return 1;
    if (id == CA_T_OBJECT)
      return 0;
    id = parent(id);
  }
}
int ca_isinstance(CaRuntime *rt, CaValue value, CaValue type) {
  if (type.tag == CA_FUNCTION_T && type.as.fn->builtin == 2)
    type = ca_type(CA_T_RANGE);
  if (type.tag == CA_TUPLE_T) {
    CaContainer *c = (CaContainer *)type.as.obj;
    for (size_t i = 0; i < c->length; i++)
      if (ca_isinstance(rt, value, c->items[i]))
        return 1;
    return 0;
  }
  if (type.tag == CA_FUNCTION_T && type.as.fn->builtin >= 4 &&
      type.as.fn->builtin <= 7) {
    int tags[] = {CA_LIST_T, CA_TUPLE_T, CA_DICT_T, CA_SET_T};
    return value.tag == tags[type.as.fn->builtin - 4];
  }
  if (value.tag == CA_INSTANCE_T)
    return ca_subclass(rt, ((CaInstance *)value.as.obj)->type, type);
  if (value.tag == CA_EXCEPTION_T)
    return ca_subclass(rt, ((CaException *)value.as.obj)->type, type);
  if (type.tag != CA_TYPE_T && type.tag != CA_CLASS_T) {
    ca_error(rt, "TypeError", "isinstance expects a type");
    return 0;
  }
  if (value.tag == CA_FOREIGN_T)
    return ca_subclass(rt, ca_type_of(value), type);
  if (type.tag == CA_CLASS_T)
    return 0;
  if (type.as.i == CA_T_OBJECT)
    return 1;
  if (type.as.i == CA_T_INT)
    return value.tag == CA_INT_T || value.tag == CA_BIG_T ||
           value.tag == CA_BOOL_T;
  if (type.as.i == CA_T_STR)
    return value.tag == CA_STR_T;
  if (type.as.i == CA_T_BOOL)
    return value.tag == CA_BOOL_T;
  if (type.as.i == CA_T_FLOAT)
    return value.tag == CA_FLOAT_T;
  if (type.as.i == CA_T_BYTES)
    return value.tag == CA_BYTES_T;
  if (type.as.i == CA_T_TYPE)
    return value.tag == CA_CLASS_T || value.tag == CA_TYPE_T ||
           (value.tag == CA_FUNCTION_T && value.as.fn->builtin >= 4 &&
            value.as.fn->builtin <= 7);
  if (type.as.i == CA_T_RANGE)
    return value.tag == CA_RANGE_T;
  if (type.as.i == CA_T_SLICE)
    return value.tag == CA_SLICE_T;
  if (type.as.i == CA_T_NONE)
    return value.tag == CA_NONE_T;
  if (type.as.i >= CA_T_LIST && type.as.i <= CA_T_SET) {
    int tags[] = {CA_LIST_T, CA_TUPLE_T, CA_DICT_T, CA_SET_T};
    return value.tag == tags[type.as.i - CA_T_LIST];
  }
  return 0;
}
CaValue ca_try_special(CaRuntime *rt, CaValue value, const char *name) {
  if (value.tag == CA_INSTANCE_T) {
    CaValue v = class_get(((CaInstance *)value.as.obj)->type, name);
    return v.tag == CA_UNBOUND_T
               ? v
               : bind(rt, v, value, ((CaInstance *)value.as.obj)->type);
  }
  return CA_UNBOUND;
}
CaValue ca_special(CaRuntime *rt, CaValue value, const char *name) {
  if (value.tag == CA_INSTANCE_T) {
    CaValue v = class_get(((CaInstance *)value.as.obj)->type, name);
    if (v.tag != CA_UNBOUND_T)
      return bind(rt, v, value, ((CaInstance *)value.as.obj)->type);
  }
  return ca_attribute(rt, value, name, 0);
}
CaValue ca_object_call(CaRuntime *rt, CaValue callable, int n, int nkw,
                       CaValue *args, const char **keys) {
  if (callable.tag == CA_BOUND_T) {
    if (n + nkw >= 128) {
      ca_error(rt, "OverflowError", "bound call exceeds native argument limit");
      return CA_UNBOUND;
    }
    CaBound *b = (CaBound *)callable.as.obj;
    CaValue values[128];
    values[0] = b->receiver;
    if (n + nkw)
      memcpy(values + 1, args, (size_t)(n + nkw) * sizeof(CaValue));
    return ca_call(rt, b->function, n + 1, nkw, values, keys);
  }
  if (callable.tag == CA_TYPE_T) {
    if (callable.as.i >= CA_T_BASEEXCEPTION && callable.as.i < 300) {
      if (nkw) {
        ca_error(rt, "TypeError",
                 "exception constructors do not accept keywords");
        return CA_UNBOUND;
      }
      return ca_exception_new(rt, callable, n, args);
    }
    return callable.as.i >= 300
               ? ca_standard_call(rt, (int)callable.as.i, n, nkw, args, keys)
               : ca_extended_call(rt, (int)callable.as.i, n, nkw, args, keys);
  }
  if (callable.tag == CA_CLASS_T) {
    if (ca_subclass(rt, callable, ca_type(CA_T_BASEEXCEPTION)))
      return ca_exception_new(rt, callable, n, args);
    CaClass *c = (CaClass *)callable.as.obj;
    if ((c->dataclass & 2) &&
        get(c->namespace_, "__init__").tag == CA_UNBOUND_T)
      return ca_dataclass_construct(
          rt, callable, n, nkw, args,
          keys); /* filled by dataclass constructor below */
    CaInstance *i = ca_allocate(rt, sizeof *i, CA_INSTANCE_T);
    if (!i)
      return CA_UNBOUND;
    i->type = callable;
    i->namespace_ = ca_container(rt, CA_DICT_T, 0, NULL);
    CaValue roots[] = {{.tag = CA_INSTANCE_T, .as.obj = (CaObj *)i},
                       CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 2);
    CaValue initializer = class_get(callable, "__init__");
    if (initializer.tag == CA_UNBOUND_T) {
      if (n || nkw)
        ca_error(rt, "TypeError", "class takes no arguments");
    } else {
      roots[1] = ca_bound(rt, roots[0], initializer);
      CaValue returned = ca_call(rt, roots[1], n, nkw, args, keys);
      if (!rt->error && returned.tag != CA_NONE_T)
        ca_error(rt, "TypeError", "__init__ must return None");
    }
    CaValue result = roots[0];
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : result;
  }
  if (callable.tag == CA_FOREIGN_T &&
      ((CaForeign *)callable.as.obj)->kind == CA_F_PARTIAL) {
    if (n + nkw >= 128) {
      ca_error(rt, "OverflowError",
               "partial invocation exceeds native argument limit");
      return CA_UNBOUND;
    }
    CaValue values[128];
    values[0] = callable;
    if (n + nkw)
      memcpy(values + 1, args, (size_t)(n + nkw) * sizeof(CaValue));
    return ca_standard_call(rt, N_PARTIAL_INVOKE, n + 1, nkw, values, keys);
  }
  if (callable.tag == CA_INSTANCE_T) {
    CaValue method = ca_special(rt, callable, "__call__");
    if (rt->error)
      return CA_UNBOUND;
    return ca_call(rt, method, n, nkw, args, keys);
  }
  ca_error(rt, "TypeError", "object is not callable");
  return CA_UNBOUND;
}
CaValue ca_type_of(CaValue value) {
  switch (value.tag) {
  case CA_NONE_T:
    return ca_type(CA_T_NONE);
  case CA_BOOL_T:
    return ca_type(CA_T_BOOL);
  case CA_INT_T:
  case CA_BIG_T:
    return ca_type(CA_T_INT);
  case CA_FLOAT_T:
    return ca_type(CA_T_FLOAT);
  case CA_SLICE_T:
    return ca_type(CA_T_SLICE);
  case CA_STR_T:
    return ca_type(CA_T_STR);
  case CA_BYTES_T:
    return ca_type(CA_T_BYTES);
  case CA_CLASS_T:
  case CA_TYPE_T:
    return ca_type(CA_T_TYPE);
  case CA_INSTANCE_T:
    return ((CaInstance *)value.as.obj)->type;
  case CA_EXCEPTION_T:
    return ((CaException *)value.as.obj)->type;
  case CA_CLOSURE_T:
    return ca_type(CA_T_FUNCTION);
  case CA_FUNCTION_T:
    if (value.as.fn->builtin == 2 ||
        (value.as.fn->builtin >= 4 && value.as.fn->builtin <= 7))
      return ca_type(CA_T_TYPE);
    return ca_type(value.as.fn->builtin ? CA_T_BUILTIN_FUNCTION
                                        : CA_T_FUNCTION);
  case CA_LIST_T:
    return ca_type(CA_T_LIST);
  case CA_TUPLE_T:
    return ca_type(CA_T_TUPLE);
  case CA_DICT_T:
    return ca_type(CA_T_DICT);
  case CA_SET_T:
    return ca_type(CA_T_SET);
  case CA_RANGE_T:
    return ca_type(CA_T_RANGE);
  case CA_BOUND_T:
    return ca_type(CA_T_METHOD);
  case CA_METHOD_T:
    return ca_type(CA_T_BUILTIN_FUNCTION);
  case CA_FOREIGN_T: {
    CaForeign *f = (CaForeign *)value.as.obj;
    return ca_type(
        f->kind == CA_F_MAP      ? CA_T_MAP
        : f->kind == CA_F_FILTER ? CA_T_FILTER
        : f->kind == CA_F_REVERSED
            ? (ca_capture(f->state, 0).tag == CA_LIST_T   ? CA_T_LIST_REVERSE
               : ca_capture(f->state, 0).tag == CA_DICT_T ? CA_T_DICT_REVERSE
                                                          : CA_T_REVERSED)
        : f->kind == CA_F_PATH     ? CA_T_PATH
        : f->kind == CA_F_DATETIME ? CA_T_DATETIME
        : f->kind == CA_F_UUID     ? CA_T_UUID
        : f->kind == CA_F_UTC      ? CA_T_TIMEZONE
                                   : CA_T_OBJECT);
  }
  default:
    return ca_type(CA_T_OBJECT);
  }
}
static int path_order(const char *a, const char *b) {
  size_t ai = 0, bi = 0;
  int afirst = 1, bfirst = 1;
  for (;;) {
    size_t as = ai, bs = bi, an = 0, bn = 0;
    if (afirst && a[0] == '/') {
      an = a[1] == '/' ? 2 : 1;
      ai = an;
    } else {
      while (a[ai] && a[ai] != '/')
        ai++;
      an = ai - as;
      if (an == 1 && a[as] == '.' && !a[ai])
        an = 0;
      if (a[ai] == '/')
        ai++;
    }
    if (bfirst && b[0] == '/') {
      bn = b[1] == '/' ? 2 : 1;
      bi = bn;
    } else {
      while (b[bi] && b[bi] != '/')
        bi++;
      bn = bi - bs;
      if (bn == 1 && b[bs] == '.' && !b[bi])
        bn = 0;
      if (b[bi] == '/')
        bi++;
    }
    afirst = bfirst = 0;
    size_t count = an < bn ? an : bn;
    int cmp = memcmp(a + as, b + bs, count);
    if (cmp)
      return cmp;
    if (an != bn)
      return an < bn ? -1 : 1;
    if (!an)
      return 0;
  }
}
CaValue ca_object_compare(CaRuntime *rt, CaValue a, CaValue b, const char *op,
                          int *handled) {
  int equal = !strcmp(op, "eq"), unequal = !strcmp(op, "ne");
  if (a.tag == CA_INSTANCE_T || b.tag == CA_INSTANCE_T) {
    const char *name = equal               ? "__eq__"
                       : unequal           ? "__ne__"
                       : !strcmp(op, "lt") ? "__lt__"
                       : !strcmp(op, "le") ? "__le__"
                       : !strcmp(op, "gt") ? "__gt__"
                                           : "__ge__";
    const char *reverse = equal               ? "__eq__"
                          : unequal           ? "__ne__"
                          : !strcmp(op, "lt") ? "__gt__"
                          : !strcmp(op, "le") ? "__ge__"
                          : !strcmp(op, "gt") ? "__lt__"
                                              : "__le__";
    CaValue method =
        a.tag == CA_INSTANCE_T ? ca_try_special(rt, a, name) : CA_UNBOUND;
    if (method.tag != CA_UNBOUND_T) {
      *handled = 1;
      return ca_call(rt, method, 1, 0, &b, NULL);
    }
    method =
        b.tag == CA_INSTANCE_T ? ca_try_special(rt, b, reverse) : CA_UNBOUND;
    if (method.tag != CA_UNBOUND_T) {
      *handled = 1;
      return ca_call(rt, method, 1, 0, &a, NULL);
    }
    if (a.tag == CA_INSTANCE_T && b.tag == CA_INSTANCE_T &&
        ((CaInstance *)a.as.obj)->type.as.obj ==
            ((CaInstance *)b.as.obj)->type.as.obj &&
        (equal || unequal)) {
      CaClass *type = (CaClass *)((CaInstance *)a.as.obj)->type.as.obj;
      if (type->dataclass & 8) {
        if (!ca_enter(rt)) {
          *handled = 1;
          return CA_UNBOUND;
        }
        int same = 1;
        CaValue roots[] = {a, b, CA_UNBOUND, CA_UNBOUND};
        CaFrame frame;
        ca_push(rt, &frame, roots, 4);
        CaContainer *names = (CaContainer *)type->fields.as.obj;
        for (size_t i = 0; i < names->length && !rt->error; i++) {
          CaString *name = (CaString *)names->items[i].as.obj;
          if (!ca_dataclass_equal_field(((CaInstance *)a.as.obj)->type,
                                        name->data))
            continue;
          roots[2] = ca_attribute(rt, a, name->data, 0);
          if (rt->error)
            break;
          roots[3] = ca_attribute(rt, b, name->data, 0);
          if (rt->error)
            break;
          CaValue comparison = ca_compare(rt, roots[2], roots[3], "eq");
          if (rt->error)
            break;
          roots[2] = comparison;
          if (!ca_truth(rt, comparison)) {
            same = 0;
            break;
          }
        }
        ca_pop(rt, &frame);
        ca_leave(rt);
        *handled = 1;
        return rt->error ? CA_UNBOUND : ca_bool(unequal ? !same : same);
      }
    }
    if (unequal) {
      method =
          a.tag == CA_INSTANCE_T ? ca_try_special(rt, a, "__eq__") : CA_UNBOUND;
      if (method.tag != CA_UNBOUND_T) {
        CaValue result = ca_call(rt, method, 1, 0, &b, NULL);
        CaFrame frame;
        ca_push(rt, &frame, &result, 1);
        int same = rt->error ? 0 : ca_truth(rt, result);
        ca_pop(rt, &frame);
        *handled = 1;
        return rt->error ? CA_UNBOUND : ca_bool(!same);
      }
    }
    if (equal || unequal) {
      int same = a.tag == b.tag && a.as.obj == b.as.obj;
      *handled = 1;
      return ca_bool(unequal ? !same : same);
    }
  }
  if (a.tag == CA_FOREIGN_T && b.tag == CA_FOREIGN_T) {
    CaForeign *x = (CaForeign *)a.as.obj, *y = (CaForeign *)b.as.obj;
    if (x->kind == y->kind &&
        (x->kind == CA_F_PATH || x->kind == CA_F_UUID ||
         x->kind == CA_F_DATETIME || x->kind == CA_F_UTC)) {
      int cmp = 0;
      if (x->kind == CA_F_PATH)
        cmp = path_order(((CaString *)x->state.as.obj)->data,
                         ((CaString *)y->state.as.obj)->data);
      else if (x->kind == CA_F_DATETIME) {
        if ((x->handle != NULL) != (y->handle != NULL)) {
          *handled = 1;
          if (equal || unequal)
            return ca_bool(unequal);
          ca_error(rt, "TypeError",
                   "cannot compare offset-naive and offset-aware datetimes");
          return CA_UNBOUND;
        }
        CaString *left = (CaString *)x->state.as.obj,
                 *right = (CaString *)y->state.as.obj;
        size_t n = left->length < right->length ? left->length : right->length;
        cmp = memcmp(left->data, right->data, n);
        if (!cmp)
          cmp = (left->length > right->length) - (left->length < right->length);
      } else if (x->kind == CA_F_UUID)
        cmp = memcmp(((CaString *)x->state.as.obj)->data,
                     ((CaString *)y->state.as.obj)->data, 36);
      *handled = 1;
      return ca_bool(equal               ? cmp == 0
                     : unequal           ? cmp != 0
                     : !strcmp(op, "lt") ? cmp < 0
                     : !strcmp(op, "le") ? cmp <= 0
                     : !strcmp(op, "gt") ? cmp > 0
                                         : cmp >= 0);
    }
  }
  if ((equal || unequal) && a.tag == b.tag) {
    int applicable = 1, same = 0;
    switch (a.tag) {
    case CA_TYPE_T:
      same = a.as.i == b.as.i;
      break;
    case CA_CLASS_T:
    case CA_CLOSURE_T:
    case CA_EXCEPTION_T:
    case CA_FOREIGN_T:
      same = a.as.obj == b.as.obj;
      break;
    case CA_BOUND_T: {
      CaBound *x = (CaBound *)a.as.obj, *y = (CaBound *)b.as.obj;
      same = x->function.tag == y->function.tag &&
             x->function.as.fn == y->function.as.fn &&
             x->receiver.tag == y->receiver.tag &&
             x->receiver.as.obj == y->receiver.as.obj;
      break;
    }
    default:
      applicable = 0;
    }
    if (applicable) {
      *handled = 1;
      return ca_bool(unequal ? !same : same);
    }
  }
  return CA_UNBOUND;
}

void ca_object_delete(CaRuntime *rt, CaValue object, const char *name) {
  CaValue mapping = CA_UNBOUND;
  if (object.tag == CA_INSTANCE_T) {
    CaValue descriptor =
        ca_class_lookup(((CaInstance *)object.as.obj)->type, name);
    if (descriptor.tag == CA_FOREIGN_T &&
        ((CaForeign *)descriptor.as.obj)->kind == CA_F_PROPERTY) {
      ca_error(rt, "AttributeError", "property has no deleter");
      return;
    }
    mapping = ((CaInstance *)object.as.obj)->namespace_;
  } else if (object.tag == CA_CLASS_T)
    mapping = ((CaClass *)object.as.obj)->namespace_;
  else {
    ca_error(rt, "AttributeError",
             "object does not support attribute deletion");
    return;
  }
  CaValue key = ca_string(rt, name, strlen(name)), args[] = {mapping, key};
  if (!rt->error)
    ca_dict_pop(rt, 2, 0, args, NULL);
  if (rt->error &&
      ca_exception_match(rt, rt->pending, ca_type(CA_T_KEYERROR))) {
    ca_exception_take(rt);
    ca_error(rt, "AttributeError", name);
  }
}

CaValue ca_class_finalize(CaRuntime *rt, CaValue value) {
  CaClass *type = (CaClass *)value.as.obj;
  if (get(type->namespace_, "__eq__").tag != CA_UNBOUND_T &&
      get(type->namespace_, "__hash__").tag == CA_UNBOUND_T)
    set(rt, type->namespace_, "__hash__", CA_NONE);
  return rt->error ? CA_UNBOUND : value;
}
