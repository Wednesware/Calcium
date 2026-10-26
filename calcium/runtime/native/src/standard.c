#include "standard.h"
#include <errno.h>
static const CaFunction functions[] = {
    {"time", 0, 0, NULL, NULL, N_TIME},
    {"monotonic", 0, 0, NULL, NULL, N_MONOTONIC},
    {"sleep", 0, 0, NULL, NULL, N_SLEEP},
    {"getcwd", 0, 0, NULL, NULL, N_GETCWD},
    {"getenv", 0, 0, NULL, NULL, N_GETENV},
    {"replace", 0, 0, NULL, NULL, N_REPLACE},
    {"unlink", 0, 0, NULL, NULL, N_UNLINK},
    {"fsync", 0, 0, NULL, NULL, N_FSYNC},
    {"fdopen", 0, 0, NULL, NULL, N_FDOPEN},
    {"path_exists", 0, 0, NULL, NULL, N_PATH_EXISTS},
    {"path_isfile", 0, 0, NULL, NULL, N_PATH_ISFILE},
    {"path_isdir", 0, 0, NULL, NULL, N_PATH_ISDIR},
    {"path_join", 0, 0, NULL, NULL, N_PATH_JOIN},
    {"path_abspath", 0, 0, NULL, NULL, N_PATH_ABSPATH},
    {"path_dirname", 0, 0, NULL, NULL, N_PATH_DIRNAME},
    {"path_basename", 0, 0, NULL, NULL, N_PATH_BASENAME},
    {"path_home", 0, 0, NULL, NULL, N_PATH_HOME},
    {"path_resolve", 0, 0, NULL, NULL, N_PATH_RESOLVE},
    {"path_expanduser", 0, 0, NULL, NULL, N_PATH_EXPANDUSER},
    {"path_mkdir", 0, 0, NULL, NULL, N_PATH_MKDIR},
    {"path_readtext", 0, 0, NULL, NULL, N_PATH_READTEXT},
    {"path_writetext", 0, 0, NULL, NULL, N_PATH_WRITETEXT},
    {"path_readbytes", 0, 0, NULL, NULL, N_PATH_READBYTES},
    {"path_writebytes", 0, 0, NULL, NULL, N_PATH_WRITEBYTES},
    {"path_is_file", 0, 0, NULL, NULL, N_PATH_IS_FILE},
    {"path_is_dir", 0, 0, NULL, NULL, N_PATH_IS_DIR},
    {"path_exists_method", 0, 0, NULL, NULL, N_PATH_EXISTS_METHOD},
    {"path_stat", 0, 0, NULL, NULL, N_PATH_STAT},
    {"path_relative", 0, 0, NULL, NULL, N_PATH_RELATIVE},
    {"path_as_posix", 0, 0, NULL, NULL, N_PATH_AS_POSIX},
    {"path_rglob", 0, 0, NULL, NULL, N_PATH_RGLOB},
    {"path_iterdir", 0, 0, NULL, NULL, N_PATH_ITERDIR},
    {"file_read", 0, 0, NULL, NULL, N_FILE_READ},
    {"file_write", 0, 0, NULL, NULL, N_FILE_WRITE},
    {"file_flush", 0, 0, NULL, NULL, N_FILE_FLUSH},
    {"file_fileno", 0, 0, NULL, NULL, N_FILE_FILENO},
    {"file_close", 0, 0, NULL, NULL, N_FILE_CLOSE},
    {"file_enter", 0, 0, NULL, NULL, N_FILE_ENTER},
    {"file_exit", 0, 0, NULL, NULL, N_FILE_EXIT},
    {"mkstemp", 0, 0, NULL, NULL, N_MKSTEMP},
    {"gettempdir", 0, 0, NULL, NULL, N_GETTEMPDIR},
    {"copytree", 0, 0, NULL, NULL, N_COPYTREE},
    {"rmtree", 0, 0, NULL, NULL, N_RMTREE},
    {"sha256", 0, 0, NULL, NULL, N_SHA256},
    {"hash_update", 0, 0, NULL, NULL, N_HASH_UPDATE},
    {"hash_digest", 0, 0, NULL, NULL, N_HASH_DIGEST},
    {"hash_hexdigest", 0, 0, NULL, NULL, N_HASH_HEXDIGEST},
    {"hash_copy", 0, 0, NULL, NULL, N_HASH_COPY},
    {"json_loads", 0, 0, NULL, NULL, N_JSON_LOADS},
    {"json_dumps", 0, 0, NULL, NULL, N_JSON_DUMPS},
    {"uuid4", 0, 0, NULL, NULL, N_UUID4},
    {"token_hex", 0, 0, NULL, NULL, N_TOKEN_HEX},
    {"token_urlsafe", 0, 0, NULL, NULL, N_TOKEN_URLSAFE},
    {"datetime_now", 0, 0, NULL, NULL, N_DATETIME_NOW},
    {"datetime_fromtimestamp", 0, 0, NULL, NULL, N_DATETIME_FROMTIMESTAMP},
    {"datetime_isoformat", 0, 0, NULL, NULL, N_DATETIME_ISOFORMAT},
    {"dataclass", 0, 0, NULL, NULL, N_DATACLASS},
    {"field", 0, 0, NULL, NULL, N_FIELD},
    {"asdict", 0, 0, NULL, NULL, N_ASDICT},
    {"partial", 0, 0, NULL, NULL, N_PARTIAL},
    {"sql_connect", 0, 0, NULL, NULL, N_SQL_CONNECT},
    {"sql_execute", 0, 0, NULL, NULL, N_SQL_EXECUTE},
    {"sql_fetchone", 0, 0, NULL, NULL, N_SQL_FETCHONE},
    {"sql_fetchall", 0, 0, NULL, NULL, N_SQL_FETCHALL},
    {"sql_commit", 0, 0, NULL, NULL, N_SQL_COMMIT},
    {"sql_rollback", 0, 0, NULL, NULL, N_SQL_ROLLBACK},
    {"sql_close", 0, 0, NULL, NULL, N_SQL_CLOSE},
    {"sql_enter", 0, 0, NULL, NULL, N_SQL_ENTER},
    {"sql_exit", 0, 0, NULL, NULL, N_SQL_EXIT},
    {"environ_get", 0, 0, NULL, NULL, N_ENVIRON_GET},
    {"text_encode", 0, 0, NULL, NULL, N_TEXT_ENCODE},
    {"text_decode", 0, 0, NULL, NULL, N_TEXT_DECODE},
    {"text_join", 0, 0, NULL, NULL, N_TEXT_JOIN},
    {"text_replace", 0, 0, NULL, NULL, N_TEXT_REPLACE},
    {"text_lower", 0, 0, NULL, NULL, N_TEXT_LOWER},
    {"text_upper", 0, 0, NULL, NULL, N_TEXT_UPPER},
    {"text_strip", 0, 0, NULL, NULL, N_TEXT_STRIP},
    {"text_lstrip", 0, 0, NULL, NULL, N_TEXT_LSTRIP},
    {"text_rstrip", 0, 0, NULL, NULL, N_TEXT_RSTRIP},
    {"text_startswith", 0, 0, NULL, NULL, N_TEXT_STARTSWITH},
    {"text_endswith", 0, 0, NULL, NULL, N_TEXT_ENDSWITH},
    {"text_split", 0, 0, NULL, NULL, N_TEXT_SPLIT},
    {"text_rsplit", 0, 0, NULL, NULL, N_TEXT_RSplit},
    {"text_find", 0, 0, NULL, NULL, N_TEXT_FIND},
    {"text_count", 0, 0, NULL, NULL, N_TEXT_COUNT},
    {"dict_items", 0, 0, NULL, NULL, N_DICT_ITEMS},
    {"dict_keys", 0, 0, NULL, NULL, N_DICT_KEYS},
    {"dict_values", 0, 0, NULL, NULL, N_DICT_VALUES},
    {"dict_fromkeys", 0, 0, NULL, NULL, N_DICT_FROMKEYS},
    {"partial_invoke", 0, 0, NULL, NULL, N_PARTIAL_INVOKE},
    {"gen_send", 0, 0, NULL, NULL, N_GEN_SEND},
    {"gen_close", 0, 0, NULL, NULL, N_GEN_CLOSE},
    {"gen_throw", 0, 0, NULL, NULL, N_GEN_THROW},
    {"sys_exit", 0, 0, NULL, NULL, N_SYS_EXIT},
    {"suppress", 0, 0, NULL, NULL, N_SUPPRESS},
    {"suppress_enter", 0, 0, NULL, NULL, N_SUPPRESS_ENTER},
    {"suppress_exit", 0, 0, NULL, NULL, N_SUPPRESS_EXIT},
    {"nullcontext", 0, 0, NULL, NULL, N_NULLCONTEXT},
    {"parser", 0, 0, NULL, NULL, N_PARSER},
    {"add_argument", 0, 0, NULL, NULL, N_ADD_ARGUMENT},
    {"parse_args", 0, 0, NULL, NULL, N_PARSE_ARGS},
    {"parse_intermixed", 0, 0, NULL, NULL, N_PARSE_INTERMIXED},
    {"parser_error", 0, 0, NULL, NULL, N_PARSER_ERROR},
    {"parser_help", 0, 0, NULL, NULL, N_PARSER_HELP},
    {"as_run", 0, 0, NULL, NULL, N_AS_RUN},
    {"as_sleep", 0, 0, NULL, NULL, N_AS_SLEEP},
    {"as_task", 0, 0, NULL, NULL, N_AS_TASK},
    {"as_wait_for", 0, 0, NULL, NULL, N_AS_WAIT_FOR},
    {"as_gather", 0, 0, NULL, NULL, N_AS_GATHER},
    {"as_lock", 0, 0, NULL, NULL, N_AS_LOCK},
    {"lock_acquire", 0, 0, NULL, NULL, N_LOCK_ACQUIRE},
    {"lock_release", 0, 0, NULL, NULL, N_LOCK_RELEASE},
    {"lock_locked", 0, 0, NULL, NULL, N_LOCK_LOCKED},
    {"lock_enter", 0, 0, NULL, NULL, N_LOCK_ENTER},
    {"lock_exit", 0, 0, NULL, NULL, N_LOCK_EXIT},
    {"task_cancel", 0, 0, NULL, NULL, N_TASK_CANCEL},
    {"task_done", 0, 0, NULL, NULL, N_TASK_DONE},
    {"task_result", 0, 0, NULL, NULL, N_TASK_RESULT},
    {"task_exception", 0, 0, NULL, NULL, N_TASK_EXCEPTION},
    {"http_server", 0, 0, NULL, NULL, N_HTTP_SERVER},
    {"http_thread_server", 0, 0, NULL, NULL, N_HTTP_THREAD_SERVER},
    {"http_serve", 0, 0, NULL, NULL, N_HTTP_SERVE},
    {"http_handle", 0, 0, NULL, NULL, N_HTTP_HANDLE},
    {"http_close", 0, 0, NULL, NULL, N_HTTP_CLOSE},
    {"http_shutdown", 0, 0, NULL, NULL, N_HTTP_SHUTDOWN},
    {"os_open", 0, 0, NULL, NULL, N_OS_OPEN},
    {"compare_digest", 0, 0, NULL, NULL, N_COMPARE_DIGEST},
    {"path_glob", 0, 0, NULL, NULL, N_PATH_GLOB},
    {"path_unlink", 0, 0, NULL, NULL, N_PATH_UNLINK},
    {"path_suffix", 0, 0, NULL, NULL, N_PATH_SUFFIX},
    {"dict_pop", 0, 0, NULL, NULL, N_DICT_POP},
};
_Static_assert(sizeof functions / sizeof functions[0] == N_LAST - 1000,
               "native function registry must match operation IDs");
CaValue ca_native_function(int id) {
  return id >= 1000 && id < N_LAST ? ca_function(functions + (id - 1000))
                                   : CA_UNBOUND;
}
CaValue ca_native_method(CaRuntime *rt, CaValue receiver, int id) {
  return ca_bound(rt, receiver, ca_native_function(id));
}
CaValue ca_native_object(CaRuntime *rt, int kind, CaValue state, void *handle,
                         long aux) {
  CaForeign *f = ca_allocate(rt, sizeof *f, CA_FOREIGN_T);
  if (!f)
    return CA_UNBOUND;
  f->kind = kind;
  f->state = state;
  f->handle = handle;
  f->auxiliary = aux;
  return (CaValue){.tag = CA_FOREIGN_T, .as.obj = (CaObj *)f};
}
const char *ca_native_text(CaRuntime *rt, CaValue value, int path) {
  if (path && value.tag == CA_FOREIGN_T &&
      ((CaForeign *)value.as.obj)->kind == CA_F_PATH)
    value = ((CaForeign *)value.as.obj)->state;
  if (value.tag != CA_STR_T) {
    ca_error(rt, "TypeError", path ? "expected str or Path" : "expected str");
    return NULL;
  }
  CaString *s = (CaString *)value.as.obj;
  if (memchr(s->data, 0, s->length)) {
    ca_error(rt, "ValueError", "embedded null character");
    return NULL;
  }
  return s->data;
}
int ca_native_arguments(CaRuntime *rt, int n, int nkw, CaValue *args,
                        const char **keys, int total, int required,
                        const char **names, CaValue *defaults, CaValue *out) {
  if (n > total) {
    ca_error(rt, "TypeError", "too many native library arguments");
    return 0;
  }
  for (int i = 0; i < total; i++)
    out[i] = i < n ? args[i] : CA_UNBOUND;
  for (int i = 0; i < nkw; i++) {
    int slot = -1;
    for (int j = 0; j < total; j++)
      if (!strcmp(names[j], keys[i])) {
        slot = j;
        break;
      }
    if (slot < 0 || out[slot].tag != CA_UNBOUND_T) {
      ca_error(rt, "TypeError",
               "unexpected or duplicate native library argument");
      return 0;
    }
    out[slot] = args[n + i];
  }
  for (int i = 0; i < total; i++)
    if (out[i].tag == CA_UNBOUND_T) {
      if (i < required || !defaults || defaults[i].tag == CA_UNBOUND_T) {
        ca_error(rt, "TypeError", "missing native library argument");
        return 0;
      }
      out[i] = defaults[i];
    }
  return 1;
}
CaValue ca_native_oserror(CaRuntime *rt, int code, const char *path) {
  char message[256];
  snprintf(message, sizeof message, "[Errno %d] %s%s%s%s", code, strerror(code),
           path ? ": '" : "", path ? path : "", path ? "'" : "");
  ca_error(rt,
           code == ENOENT                    ? "FileNotFoundError"
           : code == EEXIST                  ? "FileExistsError"
           : code == EACCES || code == EPERM ? "PermissionError"
                                             : "OSError",
           message);
  return CA_UNBOUND;
}
const char *ca_standard_type_name(long id) {
  switch (id) {
  case CA_T_PATH:
    return "pathlib.Path";
  case CA_T_DATETIME:
    return "datetime.datetime";
  case CA_T_TIMEZONE:
    return "datetime.timezone";
  case CA_T_UUID:
    return "uuid.UUID";
  default:
    return "object";
  }
}

static CaValue init_0(CaRuntime *);
static CaValue init_1(CaRuntime *);
static CaValue init_2(CaRuntime *);
static CaValue init_3(CaRuntime *);
static CaValue init_4(CaRuntime *);
static CaValue init_5(CaRuntime *);
static CaValue init_6(CaRuntime *);
static CaValue init_7(CaRuntime *);
static CaValue init_8(CaRuntime *);
static CaValue init_9(CaRuntime *);
static CaValue init_10(CaRuntime *);
static CaValue init_11(CaRuntime *);
static CaValue init_12(CaRuntime *);
static CaValue init_13(CaRuntime *);
static CaValue init_14(CaRuntime *);
static CaValue init_15(CaRuntime *);
static CaValue init_16(CaRuntime *);
static CaValue init_17(CaRuntime *);
static CaValue init_18(CaRuntime *);
static CaValue init_19(CaRuntime *);
static CaModule modules[] = {
    {.name = "os", .package = "", .initialize = init_0},
    {.name = "os.path", .package = "", .initialize = init_1},
    {.name = "pathlib", .package = "", .initialize = init_2},
    {.name = "json", .package = "", .initialize = init_3},
    {.name = "hashlib", .package = "", .initialize = init_4},
    {.name = "sqlite3", .package = "", .initialize = init_5},
    {.name = "tempfile", .package = "", .initialize = init_6},
    {.name = "shutil", .package = "", .initialize = init_7},
    {.name = "time", .package = "", .initialize = init_8},
    {.name = "uuid", .package = "", .initialize = init_9},
    {.name = "secrets", .package = "", .initialize = init_10},
    {.name = "datetime", .package = "", .initialize = init_11},
    {.name = "dataclasses", .package = "", .initialize = init_12},
    {.name = "functools", .package = "", .initialize = init_13},
    {.name = "sys", .package = "", .initialize = init_14},
    {.name = "contextlib", .package = "", .initialize = init_15},
    {.name = "argparse", .package = "", .initialize = init_16},
    {.name = "asyncio", .package = "", .initialize = init_17},
    {.name = "http", .package = "", .is_package = 1, .initialize = init_18},
    {.name = "http.server", .package = "", .initialize = init_19},
};
static CaModule *registry[] = {
    &modules[0],  &modules[1],  &modules[2],  &modules[3],  &modules[4],
    &modules[5],  &modules[6],  &modules[7],  &modules[8],  &modules[9],
    &modules[10], &modules[11], &modules[12], &modules[13], &modules[14],
    &modules[15], &modules[16], &modules[17], &modules[18], &modules[19]};
void ca_standard_setup(CaRuntime *rt) {
  rt->standard_modules = registry;
  rt->standard_count = sizeof registry / sizeof registry[0];
}
CaModule *ca_standard_find(CaRuntime *rt, const char *name) {
  (void)rt;
  for (size_t i = 0; i < sizeof registry / sizeof registry[0]; i++)
    if (!strcmp(registry[i]->name, name))
      return registry[i];
  return NULL;
}
void ca_standard_destroy(CaRuntime *rt) {
  for (size_t i = 0; i < rt->standard_count; i++) {
    CaModule *m = registry[i];
    while (m->extra) {
      CaBinding *next = m->extra->next;
      free(m->extra->name);
      free(m->extra);
      m->extra = next;
    }
    m->state = 0;
  }
  rt->standard_modules = NULL;
  rt->standard_count = 0;
}
static void put(CaRuntime *rt, int module, const char *key, CaValue value) {
  if (!rt->error)
    ca_set_attribute(rt, ca_module_value(registry[module]), key, value);
}

static CaValue init_0(CaRuntime *rt) {
  ca_extra_os(rt, ca_module_value(&modules[0]));
  put(rt, 0, "getcwd", ca_native_function(N_GETCWD));
  put(rt, 0, "getenv", ca_native_function(N_GETENV));
  put(rt, 0, "replace", ca_native_function(N_REPLACE));
  put(rt, 0, "unlink", ca_native_function(N_UNLINK));
  put(rt, 0, "remove", ca_native_function(N_UNLINK));
  put(rt, 0, "fsync", ca_native_function(N_FSYNC));
  put(rt, 0, "fdopen", ca_native_function(N_FDOPEN));
  ca_module_start(rt, &modules[1]);
  put(rt, 0, "path", ca_module_value(&modules[1]));
  put(rt, 0, "environ", ca_native_object(rt, CA_F_ENVIRON, CA_NONE, NULL, 0));
  return CA_NONE;
}
static CaValue init_1(CaRuntime *rt) {
  put(rt, 1, "exists", ca_native_function(N_PATH_EXISTS));
  put(rt, 1, "isfile", ca_native_function(N_PATH_ISFILE));
  put(rt, 1, "isdir", ca_native_function(N_PATH_ISDIR));
  put(rt, 1, "join", ca_native_function(N_PATH_JOIN));
  put(rt, 1, "abspath", ca_native_function(N_PATH_ABSPATH));
  put(rt, 1, "dirname", ca_native_function(N_PATH_DIRNAME));
  put(rt, 1, "basename", ca_native_function(N_PATH_BASENAME));
  return CA_NONE;
}
static CaValue init_2(CaRuntime *rt) {
  put(rt, 2, "Path", ca_type(CA_T_PATH));
  put(rt, 2, "PosixPath", ca_type(CA_T_PATH));
  return CA_NONE;
}
static CaValue init_3(CaRuntime *rt) {
  put(rt, 3, "JSONDecodeError",
      ca_exception_type("json.decoder.JSONDecodeError"));
  put(rt, 3, "loads", ca_native_function(N_JSON_LOADS));
  put(rt, 3, "dumps", ca_native_function(N_JSON_DUMPS));
  return CA_NONE;
}
static CaValue init_4(CaRuntime *rt) {
  put(rt, 4, "sha256", ca_native_function(N_SHA256));
  return CA_NONE;
}
static CaValue init_5(CaRuntime *rt) {
  put(rt, 5, "connect", ca_native_function(N_SQL_CONNECT));
  put(rt, 5, "Error", ca_exception_type("sqlite3.Error"));
  put(rt, 5, "DatabaseError", ca_exception_type("sqlite3.DatabaseError"));
  put(rt, 5, "OperationalError", ca_exception_type("sqlite3.OperationalError"));
  put(rt, 5, "IntegrityError", ca_exception_type("sqlite3.IntegrityError"));
  put(rt, 5, "ProgrammingError", ca_exception_type("sqlite3.ProgrammingError"));
  put(rt, 5, "InterfaceError", ca_exception_type("sqlite3.InterfaceError"));
  put(rt, 5, "DataError", ca_exception_type("sqlite3.DataError"));
  return CA_NONE;
}
static CaValue init_6(CaRuntime *rt) {
  put(rt, 6, "mkstemp", ca_native_function(N_MKSTEMP));
  put(rt, 6, "gettempdir", ca_native_function(N_GETTEMPDIR));
  return CA_NONE;
}
static CaValue init_7(CaRuntime *rt) {
  put(rt, 7, "copytree", ca_native_function(N_COPYTREE));
  put(rt, 7, "rmtree", ca_native_function(N_RMTREE));
  return CA_NONE;
}
static CaValue init_8(CaRuntime *rt) {
  put(rt, 8, "time", ca_native_function(N_TIME));
  put(rt, 8, "monotonic", ca_native_function(N_MONOTONIC));
  put(rt, 8, "perf_counter", ca_native_function(N_MONOTONIC));
  put(rt, 8, "sleep", ca_native_function(N_SLEEP));
  return CA_NONE;
}
static CaValue init_9(CaRuntime *rt) {
  put(rt, 9, "uuid4", ca_native_function(N_UUID4));
  put(rt, 9, "UUID", ca_type(CA_T_UUID));
  return CA_NONE;
}
static CaValue init_10(CaRuntime *rt) {
  put(rt, 10, "compare_digest", ca_native_function(N_COMPARE_DIGEST));
  put(rt, 10, "token_hex", ca_native_function(N_TOKEN_HEX));
  put(rt, 10, "token_urlsafe", ca_native_function(N_TOKEN_URLSAFE));
  return CA_NONE;
}
static CaValue init_11(CaRuntime *rt) {
  put(rt, 11, "datetime", ca_type(CA_T_DATETIME));
  put(rt, 11, "timezone", ca_type(CA_T_TIMEZONE));
  put(rt, 11, "UTC", ca_native_object(rt, CA_F_UTC, CA_NONE, NULL, 0));
  return CA_NONE;
}
static CaValue init_12(CaRuntime *rt) {
  put(rt, 12, "dataclass", ca_native_function(N_DATACLASS));
  put(rt, 12, "field", ca_native_function(N_FIELD));
  put(rt, 12, "asdict", ca_native_function(N_ASDICT));
  return CA_NONE;
}
static CaValue init_13(CaRuntime *rt) {
  put(rt, 13, "partial", ca_native_function(N_PARTIAL));
  return CA_NONE;
}
CaValue ca_standard_call(CaRuntime *rt, int id, int n, int nkw, CaValue *args,
                         const char **keys) {
  if (id >= N_OS_OPEN && id <= N_DICT_POP)
    return ca_extra_call(rt, id, n, nkw, args, keys);
  if (id >= N_SYS_EXIT && id <= N_PARSER_HELP)
    return ca_cli_call(rt, id, n, nkw, args, keys);
  if (id >= N_AS_RUN && id <= N_TASK_EXCEPTION)
    return ca_async_call(rt, id, n, nkw, args, keys);
  if (id >= N_HTTP_SERVER && id <= N_HTTP_SHUTDOWN)
    return ca_http_call(rt, id, n, nkw, args, keys);

  if (id == CA_T_PATH || (id >= N_PATH_HOME && id <= N_RMTREE) ||
      (id >= N_GETCWD && id <= N_PATH_BASENAME))
    return ca_io_call(rt, id, n, nkw, args, keys);
  if (id == CA_T_DATETIME || id == CA_T_TIMEZONE || id == CA_T_UUID ||
      (id >= N_TIME && id <= N_SLEEP) ||
      (id >= N_UUID4 && id <= N_ENVIRON_GET &&
       !(id >= N_DATACLASS && id <= N_SQL_EXIT)))
    return ca_system_call(rt, id, n, nkw, args, keys);
  if (id >= N_SQL_CONNECT && id <= N_SQL_EXIT)
    return ca_sql_call(rt, id, n, nkw, args, keys);
  if (id >= N_TEXT_ENCODE && id <= N_DICT_FROMKEYS)
    return ca_text_call(rt, id, n, nkw, args, keys);
  if (id == N_JSON_LOADS || id == N_JSON_DUMPS)
    return ca_json_call(rt, id, n, nkw, args, keys);
  if (id >= N_GEN_SEND && id <= N_GEN_THROW)
    return ca_generator_call(rt, id, n, nkw, args, keys);
  return ca_data_call(rt, id, n, nkw, args, keys);
}
CaValue ca_foreign_attribute(CaRuntime *rt, CaValue value, const char *name) {
  if (value.tag == CA_TYPE_T) {
    if (!strcmp(name, "__name__")) {
      const char *type = ca_type_name(value), *last = strrchr(type, '.');
      return ca_string(rt, last ? last + 1 : type,
                       strlen(last ? last + 1 : type));
    }
    if (value.as.i == CA_T_PATH && !strcmp(name, "home"))
      return ca_native_function(N_PATH_HOME);
    if (value.as.i == CA_T_DATETIME && !strcmp(name, "now"))
      return ca_native_function(N_DATETIME_NOW);
    if (value.as.i == CA_T_DATETIME && !strcmp(name, "fromtimestamp"))
      return ca_native_function(N_DATETIME_FROMTIMESTAMP);
    if (value.as.i == CA_T_TIMEZONE && !strcmp(name, "utc")) {
      CaModule *m = ca_standard_find(rt, "datetime");
      ca_module_start(rt, m);
      return ca_attribute(rt, ca_module_value(m), "UTC", 0);
    }
  } else {
    CaForeign *f = (CaForeign *)value.as.obj;
    if (f->kind == CA_F_SUPPRESS || f->kind == CA_F_NULLCONTEXT ||
        f->kind == CA_F_PARSER || f->kind == CA_F_ACTION)
      return ca_cli_attribute(rt, value, name);
    if (f->kind >= CA_F_TASK && f->kind <= CA_F_GATHER)
      return ca_async_attribute(rt, value, name);
    if (f->kind == CA_F_HTTP_SERVER)
      return ca_http_attribute(rt, value, name);
    if (f->kind == CA_F_STAT) {
      CaValue key = ca_string(rt, name, strlen(name));
      return rt->error ? CA_UNBOUND : ca_getitem(rt, f->state, key);
    }
    if (f->kind == CA_F_PATH)
      return ca_path_attribute(rt, value, name);
    if (f->kind == CA_F_FILE) {
      if (!strcmp(name, "closed"))
        return ca_bool(!f->handle);
      if (!strcmp(name, "name"))
        return ca_file_option(f, 1);
      if (!strcmp(name, "mode"))
        return ca_file_option(f, 2);
      if (!strcmp(name, "encoding")) {
        CaValue encoding = ca_file_option(f, 0);
        return (f->auxiliary & 1)          ? CA_NONE
               : encoding.tag == CA_NONE_T ? ca_string(rt, "UTF-8", 5)
                                           : encoding;
      }
      if (!strcmp(name, "errors")) {
        CaValue policy = ca_file_option(f, 4);
        return (f->auxiliary & 1)        ? CA_NONE
               : policy.tag == CA_NONE_T ? ca_string(rt, "strict", 6)
                                         : policy;
      }
      int extra = !strcmp(name, "readline")     ? 430
                  : !strcmp(name, "readlines")  ? 431
                  : !strcmp(name, "writelines") ? 432
                  : !strcmp(name, "seek")       ? 433
                  : !strcmp(name, "tell")       ? 434
                                                : 0;
      if (extra)
        return ca_more_method(rt, value, extra);
      int id = !strcmp(name, "read")        ? N_FILE_READ
               : !strcmp(name, "write")     ? N_FILE_WRITE
               : !strcmp(name, "flush")     ? N_FILE_FLUSH
               : !strcmp(name, "fileno")    ? N_FILE_FILENO
               : !strcmp(name, "close")     ? N_FILE_CLOSE
               : !strcmp(name, "__enter__") ? N_FILE_ENTER
               : !strcmp(name, "__exit__")  ? N_FILE_EXIT
                                            : 0;
      if (id)
        return ca_native_method(rt, value, id);
    }
    if (f->kind == CA_F_ENVIRON && !strcmp(name, "get"))
      return ca_native_method(rt, value, N_ENVIRON_GET);
    if (f->kind == CA_F_DATETIME && !strcmp(name, "isoformat"))
      return ca_native_method(rt, value, N_DATETIME_ISOFORMAT);
    if (f->kind == CA_F_HASH) {
      int id = !strcmp(name, "update")      ? N_HASH_UPDATE
               : !strcmp(name, "digest")    ? N_HASH_DIGEST
               : !strcmp(name, "hexdigest") ? N_HASH_HEXDIGEST
               : !strcmp(name, "copy")      ? N_HASH_COPY
                                            : 0;
      if (id)
        return ca_native_method(rt, value, id);
    }
    if (f->kind == CA_F_SQL_CONNECTION || f->kind == CA_F_SQL_CURSOR) {
      if (!strcmp(name, "rowcount"))
        return ca_int(f->auxiliary);
      int id = !strcmp(name, "execute")     ? N_SQL_EXECUTE
               : !strcmp(name, "fetchone")  ? N_SQL_FETCHONE
               : !strcmp(name, "fetchall")  ? N_SQL_FETCHALL
               : !strcmp(name, "commit")    ? N_SQL_COMMIT
               : !strcmp(name, "rollback")  ? N_SQL_ROLLBACK
               : !strcmp(name, "close")     ? N_SQL_CLOSE
               : !strcmp(name, "__enter__") ? N_SQL_ENTER
               : !strcmp(name, "__exit__")  ? N_SQL_EXIT
                                            : 0;
      if (id)
        return ca_native_method(rt, value, id);
    }
    if (f->kind == CA_F_DATETIME || f->kind == CA_F_UUID)
      return ca_system_attribute(rt, value, name);
    return ca_data_attribute(rt, value, name);
  }
  ca_error(rt, "AttributeError", name);
  return CA_UNBOUND;
}
void ca_foreign_finalize(CaForeign *f) {
  ca_http_finalize(f);
  ca_io_finalize(f);
  ca_data_finalize(f);
  ca_sql_finalize(f);
}
CaValue ca_foreign_iter(CaRuntime *rt, CaValue value) {
  CaForeign *f = (CaForeign *)value.as.obj;
  if (f->kind >= CA_F_MAP && f->kind <= CA_F_CALL_ITER)
    return value;
  if (f->kind == CA_F_FILE) {
    if (!f->handle) {
      ca_error(rt, "ValueError", "I/O operation on closed file");
      return CA_UNBOUND;
    }
    return value;
  }
  if (f->kind == CA_F_SQL_CURSOR || f->kind == CA_F_DIRECTORY_ITER ||
      f->kind == CA_F_ENUMERATE || f->kind == CA_F_ZIP)
    return value;
  if (f->kind == CA_F_DICT_ITER)
    return value;
  if (f->kind == CA_F_DICT_VIEW) {
    CaValue values[] = {f->state, ca_int(f->auxiliary),
                        ca_int(((CaContainer *)f->state.as.obj)->length)};
    CaValue state = ca_container(rt, CA_TUPLE_T, 3, values);
    return rt->error ? CA_UNBOUND
                     : ca_native_object(rt, CA_F_DICT_ITER, state, NULL, 0);
  }
  ca_error(rt, "TypeError", "native object is not iterable");
  return CA_UNBOUND;
}
int ca_foreign_next(CaRuntime *rt, CaValue value, CaValue *out) {
  CaForeign *f = (CaForeign *)value.as.obj;
  if (f->kind >= CA_F_MAP && f->kind <= CA_F_CALL_ITER)
    return ca_builtin_iterator_next(rt, value, out);
  if (f->kind == CA_F_FILE) {
    *out = ca_file_read(rt, value, -1, 1);
    return rt->error ? -1 : ((CaString *)out->as.obj)->length ? 1 : 0;
  }
  if (f->kind == CA_F_ENUMERATE || f->kind == CA_F_ZIP) {
    if (f->kind == CA_F_ZIP && f->auxiliary)
      return 0;
    CaValue roots[] = {value, CA_UNBOUND, CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 3);
    CaContainer *state = (CaContainer *)f->state.as.obj;
    int status = 1;
    if (f->kind == CA_F_ENUMERATE) {
      status = ca_next(rt, state->items[0], &roots[1]);
      if (status > 0) {
        CaValue pair[] = {state->items[1], roots[1]};
        roots[2] = ca_container(rt, CA_TUPLE_T, 2, pair);
        if (!rt->error)
          state->items[1] = ca_numeric(rt, state->items[1], ca_int(1), '+');
      }
    } else if (!state->length)
      status = 0;
    else {
      roots[2] = ca_container(rt, CA_LIST_T, 0, NULL);
      for (size_t i = 0; i < state->length && !rt->error; i++) {
        status = ca_next(rt, state->items[i], &roots[1]);
        if (status <= 0)
          break;
        ca_append(rt, roots[2], roots[1]);
      }
      if (!rt->error && status > 0) {
        CaContainer *items = (CaContainer *)roots[2].as.obj;
        roots[2] = ca_container(rt, CA_TUPLE_T, items->length, items->items);
      }
      if (!status)
        f->auxiliary = 1;
    }
    *out = status > 0 ? roots[2] : CA_UNBOUND;
    ca_pop(rt, &frame);
    return rt->error ? -1 : status;
  }
  if (f->kind == CA_F_DICT_ITER) {
    CaValue mapping = ca_capture(f->state, 0);
    CaContainer *c = (CaContainer *)mapping.as.obj;
    int64_t size = ca_capture(f->state, 2).as.i;
    if ((int64_t)c->length != size) {
      ca_error(rt, "RuntimeError", "dictionary changed size during iteration");
      return -1;
    }
    if ((size_t)f->auxiliary >= c->length) {
      *out = CA_UNBOUND;
      return 0;
    }
    CaValue *item = c->items + f->auxiliary;
    long kind = ca_capture(f->state, 1).as.i;
    *out = kind == 2 ? ca_container(rt, CA_TUPLE_T, 2, item)
                     : item[kind == 1 ? 1 : 0];
    f->auxiliary += 2;
    return rt->error ? -1 : 1;
  }
  if (f->kind == CA_F_SQL_CURSOR)
    return ca_sql_next(rt, value, out);
  if (f->kind == CA_F_DIRECTORY_ITER)
    return ca_io_next(rt, value, out);
  ca_error(rt, "TypeError", "native object is not an iterator");
  return -1;
}
void ca_foreign_render(CaRuntime *rt, FILE *stream, CaValue value, int mode) {
  CaForeign *f = (CaForeign *)value.as.obj;
  if (f->kind == CA_F_PATH) {
    if (mode)
      fputs("PosixPath(", stream);
    CaValue text = ca_stringify(rt, f->state, mode ? 1 : 0);
    if (!rt->error) {
      CaString *s = (CaString *)text.as.obj;
      fwrite(s->data, 1, s->length, stream);
    }
    if (mode)
      fputc(')', stream);
    return;
  }
  if (f->kind == CA_F_UUID || f->kind == CA_F_DATETIME) {
    CaString *s = (CaString *)f->state.as.obj;
    fwrite(s->data, 1, s->length, stream);
    return;
  }
  if (f->kind == CA_F_UTC) {
    fputs(mode ? "datetime.timezone.utc" : "UTC", stream);
    return;
  }
  fputs("<native object>", stream);
}

void ca_standard_arguments(CaRuntime *rt, int argc, char **argv) {
  rt->arguments = ca_container(rt, CA_LIST_T, 0, NULL);
  for (int i = 0; i < argc && !rt->error; i++)
    ca_append(rt, rt->arguments, ca_string(rt, argv[i], strlen(argv[i])));
}
static CaValue init_14(CaRuntime *rt) {
  put(rt, 14, "argv", rt->arguments);
  put(rt, 14, "exit", ca_native_function(N_SYS_EXIT));
  put(rt, 14, "platform", ca_string(rt, "linux", 5));
  return CA_NONE;
}
static CaValue init_15(CaRuntime *rt) {
  put(rt, 15, "suppress", ca_native_function(N_SUPPRESS));
  put(rt, 15, "nullcontext", ca_native_function(N_NULLCONTEXT));
  return CA_NONE;
}
static CaValue init_16(CaRuntime *rt) {
  put(rt, 16, "ArgumentParser", ca_native_function(N_PARSER));
  CaValue cls = ca_class(rt, "Namespace", 0, NULL);
  if (!rt->error) {
    ca_set_attribute(rt, cls, "__module__", ca_string(rt, "argparse", 8));
    put(rt, 16, "Namespace", cls);
  }
  return CA_NONE;
}
static CaValue init_17(CaRuntime *rt) {
  const char *names[] = {"run",      "sleep",  "create_task",
                         "wait_for", "gather", "Lock"};
  int ids[] = {N_AS_RUN,      N_AS_SLEEP,  N_AS_TASK,
               N_AS_WAIT_FOR, N_AS_GATHER, N_AS_LOCK};
  for (size_t i = 0; i < 6; i++)
    put(rt, 17, names[i], ca_native_function(ids[i]));
  put(rt, 17, "CancelledError", ca_type(CA_T_CANCELLEDERROR));
  put(rt, 17, "TimeoutError", ca_type(CA_T_TIMEOUTERROR));
  return CA_NONE;
}
static CaValue init_18(CaRuntime *rt) {
  (void)rt;
  return CA_NONE;
}
static CaValue init_19(CaRuntime *rt) {
  put(rt, 19, "HTTPServer", ca_native_function(N_HTTP_SERVER));
  put(rt, 19, "ThreadingHTTPServer", ca_native_function(N_HTTP_THREAD_SERVER));
  CaValue cls = ca_class(rt, "SimpleHTTPRequestHandler", 0, NULL);
  if (!rt->error) {
    ca_set_attribute(rt, cls, "__module__", ca_string(rt, "http.server", 11));
    put(rt, 19, "SimpleHTTPRequestHandler", cls);
  }
  return CA_NONE;
}
