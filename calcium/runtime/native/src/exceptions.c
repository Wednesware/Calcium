#include "standard.h"
static const char *names[] = {"BaseException",
                              "Exception",
                              "TypeError",
                              "ValueError",
                              "RuntimeError",
                              "NameError",
                              "UnboundLocalError",
                              "AttributeError",
                              "LookupError",
                              "KeyError",
                              "IndexError",
                              "OSError",
                              "FileNotFoundError",
                              "FileExistsError",
                              "PermissionError",
                              "ImportError",
                              "ModuleNotFoundError",
                              "ZeroDivisionError",
                              "OverflowError",
                              "MemoryError",
                              "RecursionError",
                              "StopIteration",
                              "AssertionError",
                              "NotImplementedError",
                              "SystemExit",
                              "KeyboardInterrupt",
                              "json.decoder.JSONDecodeError",
                              "sqlite3.Error",
                              "sqlite3.DatabaseError",
                              "sqlite3.OperationalError",
                              "sqlite3.IntegrityError",
                              "sqlite3.ProgrammingError",
                              "sqlite3.InterfaceError",
                              "sqlite3.DataError",
                              "UnicodeError",
                              "UnicodeEncodeError",
                              "UnicodeDecodeError", "asyncio.exceptions.CancelledError", "TimeoutError"};
const char *ca_type_name(CaValue type) {
  if (type.tag == CA_CLASS_T)
    return ((CaString *)((CaClass *)type.as.obj)->name.as.obj)->data;
  if (type.tag == CA_TYPE_T) {
    if (type.as.i >= 300)
      return ca_standard_type_name(type.as.i);
    if (type.as.i >= 200 &&
        type.as.i < 200 + (long)(sizeof names / sizeof names[0]))
      return names[type.as.i - 200];
    switch (type.as.i) {
    case CA_T_STR:
      return "str";
    case CA_T_INT:
      return "int";
    case CA_T_FLOAT:
      return "float";
    case CA_T_BOOL:
      return "bool";
    case CA_T_BYTES:
      return "bytes";
    case CA_T_NONE:
      return "NoneType";
    case CA_T_FUNCTION:
      return "function";
    case CA_T_BUILTIN_FUNCTION:
      return "builtin_function_or_method";
    case CA_T_METHOD:
      return "method";
    case CA_T_TYPE:
      return "type";
    case CA_T_LIST:
      return "list";
    case CA_T_TUPLE:
      return "tuple";
    case CA_T_DICT:
      return "dict";
    case CA_T_SET:
      return "set";
    case CA_T_RANGE:
      return "range";
    default:
      return "object";
    }
  }
  return "object";
}
CaValue ca_exception_type(const char *name) {
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
    if (!strcmp(name, names[i]))
      return ca_type(200 + (long)i);
  return CA_UNBOUND;
}
CaValue ca_exception_new(CaRuntime *rt, CaValue type, int n, CaValue *args) {
  CaException *e = ca_allocate(rt, sizeof *e, CA_EXCEPTION_T);
  if (!e)
    return CA_UNBOUND;
  e->type = type;
  e->args = ca_container(rt, CA_TUPLE_T, (size_t)n, args);
  e->message = CA_UNBOUND;
  e->cause = CA_NONE;
  e->context = rt->handled.tag == CA_EXCEPTION_T ? rt->handled : CA_NONE;
  return rt->error ? CA_UNBOUND
                   : (CaValue){.tag = CA_EXCEPTION_T, .as.obj = (CaObj *)e};
}
CaValue ca_error_get(CaRuntime *rt) {
  return rt->error ? rt->pending : CA_NONE;
}
CaValue ca_exception_take(CaRuntime *rt) {
  CaValue value = ca_error_get(rt);
  rt->pending = CA_NONE;
  rt->error = NULL;
  rt->message[0] = 0;
  return value;
}
void ca_raise(CaRuntime *rt, CaValue value, CaValue cause, int explicit_cause) {
  rt->error = NULL;
  rt->pending = CA_NONE;
  if (value.tag == CA_UNBOUND_T) {
    if (rt->handled.tag != CA_EXCEPTION_T) {
      ca_error(rt, "RuntimeError", "No active exception to reraise");
      return;
    }
    value = rt->handled;
  }
  if (value.tag == CA_CLASS_T || value.tag == CA_TYPE_T) {
    if (!ca_subclass(rt, value, ca_type(CA_T_BASEEXCEPTION))) {
      if (!rt->error)
        ca_error(rt, "TypeError", "exceptions must derive from BaseException");
      return;
    }
    value = ca_exception_new(rt, value, 0, NULL);
  }
  if (rt->error)
    return;
  if (value.tag != CA_EXCEPTION_T) {
    ca_error(rt, "TypeError", "exceptions must derive from BaseException");
    return;
  }
  CaException *e = (CaException *)value.as.obj;
  if (explicit_cause) {
    if (cause.tag == CA_CLASS_T || cause.tag == CA_TYPE_T)
      cause = ca_exception_new(rt, cause, 0, NULL);
    if (cause.tag != CA_NONE_T && cause.tag != CA_EXCEPTION_T) {
      ca_error(rt, "TypeError",
               "exception causes must derive from BaseException");
      return;
    }
    e->cause = cause;
  }
  CaValue message = ca_stringify(rt, value, 0);
  if (rt->error)
    return;
  CaString *text = (CaString *)message.as.obj;
  snprintf(rt->message, sizeof rt->message, "%s", text->data);
  rt->pending = value;
  rt->error = ca_type_name(e->type);
}
void ca_error_set(CaRuntime *rt, CaValue value) {
  rt->error = NULL;
  rt->pending = CA_NONE;
  rt->message[0] = 0;
  if (value.tag != CA_UNBOUND_T && value.tag != CA_NONE_T)
    ca_raise(rt, value, CA_NONE, 0);
}
int ca_exception_match(CaRuntime *rt, CaValue exception, CaValue type) {
  if (type.tag == CA_TUPLE_T) {
    CaContainer *c = (CaContainer *)type.as.obj;
    for (size_t i = 0; i < c->length; i++)
      if (ca_exception_match(rt, exception, c->items[i]))
        return 1;
    return 0;
  }
  if ((type.tag != CA_CLASS_T && type.tag != CA_TYPE_T) ||
      !ca_subclass(rt, type, ca_type(CA_T_BASEEXCEPTION))) {
    if (!rt->error)
      ca_error(rt, "TypeError",
               "catching classes that do not inherit from BaseException is not "
               "allowed");
    return 0;
  }
  return exception.tag == CA_EXCEPTION_T &&
         ca_subclass(rt, ((CaException *)exception.as.obj)->type, type);
}
CaValue ca_with_exit(CaRuntime *rt, CaValue exit, CaValue exception) {
  CaValue type = exception.tag == CA_EXCEPTION_T
                     ? ((CaException *)exception.as.obj)->type
                     : CA_NONE;
  CaValue args[] = {type, exception, CA_NONE};
  return ca_call(rt, exit, 3, 0, args, NULL);
}
int ca_exit_status(CaRuntime *rt) {
  if (!rt->error)
    return 0;
  if (rt->pending.tag == CA_EXCEPTION_T &&
      ca_subclass(rt, ((CaException *)rt->pending.as.obj)->type,
                  ca_type(CA_T_SYSTEMEXIT))) {
    CaValue exception = ca_exception_take(rt);
    CaValue args = ((CaException *)exception.as.obj)->args;
    CaContainer *items = (CaContainer *)args.as.obj;
    CaValue code = !items->length       ? CA_NONE
                   : items->length == 1 ? items->items[0]
                                        : args;
    if (code.tag == CA_NONE_T)
      return 0;
    if (code.tag == CA_INT_T || code.tag == CA_BOOL_T)
      return (int)((uint64_t)code.as.i & 255);
    if (code.tag == CA_BIG_T)
      return 255;
    CaFrame frame;
    ca_push(rt, &frame, &exception, 1);
    CaValue message = ca_stringify(rt, code, 0);
    if (!rt->error) {
      CaString *s = (CaString *)message.as.obj;
      fwrite(s->data, 1, s->length, stderr);
      fputc('\n', stderr);
    }
    ca_pop(rt, &frame);
    return 1;
  }
  fprintf(stderr, "%s: %s\n", rt->error, rt->message);
  return 1;
}
