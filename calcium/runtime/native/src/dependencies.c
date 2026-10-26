#define _GNU_SOURCE
#include "dependencies.h"
static const CaFunction functions[] = {
#define DEP_FUNCTION(op, label) {.name = label, .builtin = op},
    CA_DEPENDENCY_FUNCTIONS(DEP_FUNCTION)
#undef DEP_FUNCTION
};
int ca_dependency_available(const char *name) {
#ifndef CA_WITH_PCRE2
  if (!strcmp(name, "re"))
    return 0;
#endif
#ifndef CA_WITH_ARCHIVE
  if (!strcmp(name, "zipfile"))
    return 0;
#endif
#ifndef CA_WITH_CURL
  if (!strcmp(name, "urllib.request"))
    return 0;
#endif
#ifndef CA_WITH_THREADS
  if (!strcmp(name, "threading"))
    return 0;
#endif
#ifndef CA_WITH_OPENSSL
  if (!strcmp(name, "hashlib"))
    return 0;
#endif
#ifndef CA_WITH_SQLITE
  if (!strcmp(name, "sqlite3"))
    return 0;
#endif
  (void)name;
  return 1;
}
CaValue ca_dependency_function(int id) {
  return id >= 2000 && id < D_LAST ? ca_function(&functions[id - 2000])
                                   : CA_UNBOUND;
}
CaValue ca_dep_get(CaValue object, const char *name) {
  CaValue mapping = object;
  if (object.tag == CA_EXCEPTION_T)
    mapping = ((CaException *)object.as.obj)->attributes;
  if (object.tag == CA_FOREIGN_T)
    mapping = ((CaForeign *)object.as.obj)->state;
  if (object.tag == CA_INSTANCE_T)
    mapping = ((CaInstance *)object.as.obj)->namespace_;
  return ca_namespace_get(mapping, name);
}
void ca_dep_put(CaRuntime *rt, CaValue object, const char *name,
                CaValue value) {
  if (object.tag == CA_EXCEPTION_T) {
    CaException *e = (CaException *)object.as.obj;
    if (e->attributes.tag != CA_DICT_T)
      e->attributes = ca_container(rt, CA_DICT_T, 0, NULL);
    object = e->attributes;
  }
  if (object.tag == CA_FOREIGN_T)
    object = ((CaForeign *)object.as.obj)->state;
  if (object.tag == CA_INSTANCE_T)
    object = ((CaInstance *)object.as.obj)->namespace_;
  if (object.tag == CA_DICT_T)
    ca_namespace_set(rt, object, name, value);
  else
    ca_set_attribute(rt, object, name, value);
}
CaValue ca_dep_record(CaRuntime *rt, const char *name) {
  CaValue type = ca_class(rt, name, 0, NULL);
  return rt->error ? CA_UNBOUND : ca_call(rt, type, 0, 0, NULL, NULL);
}
CaValue ca_dep_error(CaRuntime *rt, const char *type, const char *message) {
  ca_error(rt, type, message);
  return CA_UNBOUND;
}
static void function(CaRuntime *rt, CaValue m, const char *name, int id) {
  ca_dep_put(rt, m, name, ca_dependency_function(id));
}
static void error(CaRuntime *rt, CaValue m, const char *name,
                  const char *type) {
  ca_dep_put(rt, m, name, ca_exception_type(type));
}
CaValue ca_dependency_init(CaRuntime *rt, int id, CaValue m) {
  switch (id) {
  case 0:
    function(rt, m, "run", D_PROCESS_RUN);
    function(rt, m, "Popen", D_PROCESS_POPEN);
    function(rt, m, "check_call", D_PROCESS_CHECK_CALL);
    function(rt, m, "check_output", D_PROCESS_CHECK_OUTPUT);
    error(rt, m, "SubprocessError", "subprocess.SubprocessError");
    error(rt, m, "CalledProcessError", "subprocess.CalledProcessError");
    error(rt, m, "TimeoutExpired", "subprocess.TimeoutExpired");
    ca_dep_put(rt, m, "PIPE", ca_int(-1));
    ca_dep_put(rt, m, "STDOUT", ca_int(-2));
    ca_dep_put(rt, m, "DEVNULL", ca_int(-3));
    break;
  case 1:
    function(rt, m, "ZipFile", D_ARCHIVE_ZIPFILE);
    function(rt, m, "is_zipfile", D_ARCHIVE_IS_ZIPFILE);
    error(rt, m, "BadZipFile", "zipfile.BadZipFile");
    ca_dep_put(rt, m, "ZIP_STORED", ca_int(0));
    ca_dep_put(rt, m, "ZIP_DEFLATED", ca_int(8));
    break;
  case 2: {
    const char *names[] = {"compile", "match",    "search", "fullmatch",
                           "findall", "finditer", "split",  "sub",
                           "subn",    "escape"};
    for (int i = 0; i < 10; i++)
      function(rt, m, names[i], D_RE_COMPILE + i);
    const char *flags[] = {"NOFLAG", "I", "IGNORECASE", "M", "MULTILINE", "S",
                           "DOTALL", "X", "VERBOSE",    "A", "ASCII",     "U",
                           "UNICODE"};
    int values[] = {0, 2, 2, 8, 8, 16, 16, 64, 64, 256, 256, 32, 32};
    for (int i = 0; i < 13; i++)
      ca_dep_put(rt, m, flags[i], ca_int(values[i]));
    error(rt, m, "error", "re.PatternError");
    error(rt, m, "PatternError", "re.PatternError");
    break;
  }
  case 4:
    error(rt, m, "URLError", "urllib.error.URLError");
    error(rt, m, "HTTPError", "urllib.error.HTTPError");
    break;
  case 5:
    function(rt, m, "urlretrieve", D_URL_RETRIEVE);
    function(rt, m, "urlopen", D_URL_OPEN);
    break;
  case 6:
    function(rt, m, "format_exc", D_TRACE_FORMAT_EXC);
    function(rt, m, "format_exception", D_TRACE_FORMAT_EXCEPTION);
    function(rt, m, "format_exception_only", D_TRACE_FORMAT_ONLY);
    function(rt, m, "print_exc", D_TRACE_PRINT_EXC);
    function(rt, m, "print_exception", D_TRACE_PRINT_EXCEPTION);
    break;
  case 7:
    function(rt, m, "import_module", D_IMPORT_MODULE);
    function(rt, m, "invalidate_caches", D_IMPORT_INVALIDATE);
    break;
  case 8:
    function(rt, m, "find_spec", D_IMPORT_FIND_SPEC);
    function(rt, m, "spec_from_file_location", D_IMPORT_SPEC_FILE);
    function(rt, m, "module_from_spec", D_IMPORT_MODULE_SPEC);
    break;
  case 9:
    function(rt, m, "Thread", D_THREAD_THREAD);
    function(rt, m, "Lock", D_THREAD_LOCK);
    function(rt, m, "RLock", D_THREAD_RLOCK);
    function(rt, m, "Event", D_THREAD_EVENT);
    function(rt, m, "get_ident", D_THREAD_IDENT);
    function(rt, m, "current_thread", D_THREAD_CURRENT);
    function(rt, m, "active_count", D_THREAD_ACTIVE);
    break;
  }
  return rt->error ? CA_UNBOUND : CA_NONE;
}
CaValue ca_dependency_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                           const char **k) {
  if (id >= D_RE_COMPILE && id <= D_RE_EXPAND)
    return ca_re_call(rt, id, n, nkw, a, k);
  if (id >= D_PROCESS_RUN && id <= D_PROCESS_CHECK_OUTPUT)
    return ca_process_call(rt, id, n, nkw, a, k);
  if (id >= D_ARCHIVE_ZIPFILE && id <= D_ARCHIVE_TESTZIP)
    return ca_archive_call(rt, id, n, nkw, a, k);
  if (id >= D_URL_RETRIEVE && id <= D_URL_GETURL)
    return ca_url_call(rt, id, n, nkw, a, k);
  if (id >= D_IMPORT_MODULE && id <= D_IMPORT_INVALIDATE)
    return ca_import_call(rt, id, n, nkw, a, k);
  if (id >= D_TRACE_FORMAT_EXC && id <= D_TRACE_PRINT_EXCEPTION)
    return ca_trace_call(rt, id, n, nkw, a, k);
  if (id >= D_THREAD_THREAD && id <= D_THREAD_ACTIVE)
    return ca_thread_call(rt, id, n, nkw, a, k);
  if (id == D_HELPERS_RUNNING_LOOP) {
    if (n || nkw)
      return ca_dep_error(rt, "TypeError",
                          "get_running_loop takes no arguments");
    return ca_async_running_loop(rt);
  }
  if (id == D_HELPERS_LOOP_RUNNING) {
    if (n != 1 || nkw)
      return ca_dep_error(rt, "TypeError", "is_running takes no arguments");
    return ca_dep_get(a[0], "_running");
  }
  if (id == D_HELPERS_JSON_LOAD) {
    if (n != 1)
      return ca_dep_error(rt, "TypeError", "json.load requires a file");
    CaValue roots[] = {a[0], CA_UNBOUND};
    CaFrame f;
    ca_push(rt, &f, roots, 2);
    roots[1] = ca_attribute(rt, a[0], "read", 0);
    roots[1] = ca_call(rt, roots[1], 0, 0, NULL, NULL);
    CaValue *args = calloc((size_t)nkw + 1, sizeof(CaValue));
    if (!args) {
      ca_pop(rt, &f);
      return ca_dep_error(rt, "MemoryError", "JSON argument allocation failed");
    }
    args[0] = roots[1];
    for (int i = 0; i < nkw; i++)
      args[1 + i] = a[n + i];
    CaValue result = rt->error
                         ? CA_UNBOUND
                         : ca_json_call(rt, N_JSON_LOADS, 1, nkw, args, k);
    free(args);
    ca_pop(rt, &f);
    return result;
  }
  if (id == D_HELPERS_JSON_DUMP) {
    if (n != 2)
      return ca_dep_error(rt, "TypeError",
                          "json.dump requires an object and file");
    CaValue *args = calloc((size_t)nkw + 1, sizeof(CaValue));
    if (!args)
      return ca_dep_error(rt, "MemoryError", "JSON argument allocation failed");
    args[0] = a[0];
    for (int i = 0; i < nkw; i++)
      args[i + 1] = a[n + i];
    CaValue roots[] = {a[1], ca_json_call(rt, N_JSON_DUMPS, 1, nkw, args, k),
                       CA_UNBOUND};
    free(args);
    CaFrame f;
    ca_push(rt, &f, roots, 3);
    roots[2] = ca_attribute(rt, a[1], "write", 0);
    if (!rt->error)
      ca_call(rt, roots[2], 1, 0, &roots[1], NULL);
    ca_pop(rt, &f);
    return rt->error ? CA_UNBOUND : CA_NONE;
  }
  return ca_dep_error(rt, "NotImplementedError",
                      "unknown native library operation");
}
CaValue ca_dependency_attribute(CaRuntime *rt, CaValue v, const char *name) {
  CaForeign *f = (CaForeign *)v.as.obj;
  if (f->kind == CA_F_REGEX || f->kind == CA_F_MATCH)
    return ca_re_attribute(rt, v, name);
  if (f->kind == CA_F_PROCESS)
    return ca_process_attribute(rt, v, name);
  if (f->kind == CA_F_ZIPFILE)
    return ca_archive_attribute(rt, v, name);
  if (f->kind == CA_F_URL)
    return ca_url_attribute(rt, v, name);
  if (f->kind >= CA_F_THREAD && f->kind <= CA_F_EVENT)
    return ca_thread_attribute(rt, v, name);
  return ca_dep_error(rt, "AttributeError", name);
}
void ca_dependency_set(CaRuntime *rt, CaValue v, const char *name,
                       CaValue value) {
  if (((CaForeign *)v.as.obj)->kind == CA_F_THREAD && !strcmp(name, "daemon")) {
    if (ca_dep_get(v, "_started").as.i) {
      ca_error(rt, "RuntimeError", "cannot set daemon status of active thread");
      return;
    }
    ca_dep_put(rt, v, name, ca_bool(ca_truth(rt, value)));
    return;
  }
  ca_error(rt, "AttributeError", "native resource attribute is read-only");
}
void ca_dependency_finalize(CaForeign *f) {
  ca_re_finalize(f);
  ca_process_finalize(f);
  ca_archive_finalize(f);
  ca_thread_finalize(f);
}

void ca_dependency_exception(CaRuntime *rt, CaValue v, int n, CaValue *a) {
  long type = ((CaException *)v.as.obj)->type.as.i;
  const char *names[5] = {0};
  int count = 0;
  if (type == CA_T_CALLEDPROCESSERROR) {
    names[0] = "returncode";
    names[1] = "cmd";
    names[2] = "output";
    names[3] = "stderr";
    count = 4;
  }
  if (type == CA_T_TIMEOUTEXPIRED) {
    names[0] = "cmd";
    names[1] = "timeout";
    names[2] = "output";
    names[3] = "stderr";
    count = 4;
  }
  if (type == CA_T_URLERROR) {
    names[0] = "reason";
    count = 1;
  }
  if (type == CA_T_HTTPERROR) {
    names[0] = "url";
    names[1] = "code";
    names[2] = "reason";
    names[3] = "headers";
    names[4] = "fp";
    count = 5;
  }
  for (int i = 0; i < count && !rt->error; i++)
    ca_dep_put(rt, v, names[i], i < n ? a[i] : CA_NONE);
  if (type == CA_T_CALLEDPROCESSERROR || type == CA_T_TIMEOUTEXPIRED)
    ca_dep_put(rt, v, "stdout", n > 2 ? a[2] : CA_NONE);
}

void ca_dependency_exception_message(CaRuntime *rt, CaValue v, int n,
                                     CaValue *a) {
  CaException *e = (CaException *)v.as.obj;
  long type = e->type.as.i;
  if (type != CA_T_HTTPERROR && type != CA_T_URLERROR &&
      type != CA_T_CALLEDPROCESSERROR && type != CA_T_TIMEOUTEXPIRED)
    return;
  CaValue roots[] = {v, CA_UNBOUND, CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 3);
  char *message = NULL;
  if (type == CA_T_URLERROR && n >= 1) {
    roots[1] = ca_stringify(rt, a[0], 0);
    if (!rt->error && asprintf(&message, "<urlopen error %s>",
                               ((CaString *)roots[1].as.obj)->data) < 0)
      message = NULL;
  }
  if (type == CA_T_HTTPERROR && n >= 3 && a[1].tag == CA_INT_T) {
    roots[1] = ca_stringify(rt, a[2], 0);
    if (!rt->error &&
        asprintf(&message, "HTTP Error %lld: %s", (long long)a[1].as.i,
                 ((CaString *)roots[1].as.obj)->data) < 0)
      message = NULL;
  }
  if (type == CA_T_CALLEDPROCESSERROR && n >= 2 && a[0].tag == CA_INT_T &&
      a[0].as.i >= 0) {
    roots[1] = ca_stringify(rt, a[1], 0);
    if (!rt->error &&
        asprintf(&message, "Command '%s' returned non-zero exit status %lld.",
                 ((CaString *)roots[1].as.obj)->data, (long long)a[0].as.i) < 0)
      message = NULL;
  }
  if (type == CA_T_TIMEOUTEXPIRED && n >= 2) {
    roots[1] = ca_stringify(rt, a[0], 0);
    roots[2] = rt->error ? CA_UNBOUND : ca_stringify(rt, a[1], 0);
    if (!rt->error &&
        asprintf(&message, "Command '%s' timed out after %s seconds",
                 ((CaString *)roots[1].as.obj)->data,
                 ((CaString *)roots[2].as.obj)->data) < 0)
      message = NULL;
  }
  if (message && !rt->error)
    e->message = ca_string(rt, message, strlen(message));
  free(message);
  ca_pop(rt, &frame);
}
