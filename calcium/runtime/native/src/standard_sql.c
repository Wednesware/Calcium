#define _GNU_SOURCE
#include "standard.h"
#ifdef CA_WITH_SQLITE
#include <ctype.h>
#include <limits.h>
#include <sqlite3.h>
#include <strings.h>
typedef struct {
  sqlite3_stmt *statement;
  int done;
} Cursor;
static CaValue fail(CaRuntime *rt, sqlite3 *db, int status) {
  const char *name =
      (status & 255) == SQLITE_CONSTRAINT ? "sqlite3.IntegrityError"
      : (status & 255) == SQLITE_MISUSE || (status & 255) == SQLITE_RANGE
          ? "sqlite3.ProgrammingError"
      : (status & 255) == SQLITE_MISMATCH ? "sqlite3.InterfaceError"
      : (status & 255) == SQLITE_NOMEM    ? "MemoryError"
                                          : "sqlite3.OperationalError";
  ca_error(rt, name, db ? sqlite3_errmsg(db) : sqlite3_errstr(status));
  return CA_UNBOUND;
}
static sqlite3 *connection(CaRuntime *rt, CaValue value) {
  if (value.tag != CA_FOREIGN_T ||
      ((CaForeign *)value.as.obj)->kind != CA_F_SQL_CONNECTION) {
    ca_error(rt, "TypeError", "expected a native SQLite connection");
    return NULL;
  }
  sqlite3 *db = ((CaForeign *)value.as.obj)->handle;
  if (!db)
    ca_error(rt, "sqlite3.ProgrammingError",
             "Cannot operate on a closed database.");
  return db;
}
static int dml(const char *sql) {
  for (;;) {
    while (isspace((unsigned char)*sql))
      sql++;
    if (sql[0] == '-' && sql[1] == '-') {
      sql += 2;
      while (*sql && *sql != '\n')
        sql++;
      continue;
    }
    if (sql[0] == '/' && sql[1] == '*') {
      const char *end = strstr(sql + 2, "*/");
      if (!end)
        return 0;
      sql = end + 2;
      continue;
    }
    break;
  }
  return !strncasecmp(sql, "insert", 6) || !strncasecmp(sql, "update", 6) ||
         !strncasecmp(sql, "delete", 6) || !strncasecmp(sql, "replace", 7);
}
static CaValue row(CaRuntime *rt, sqlite3_stmt *statement) {
  int count = sqlite3_column_count(statement);
  CaValue result = ca_container(rt, CA_TUPLE_T, 0, NULL);
  if (rt->error)
    return CA_UNBOUND;
  CaValue *values = calloc((size_t)count ? (size_t)count : 1, sizeof *values);
  if (!values) {
    ca_error(rt, "MemoryError", "SQLite row allocation failed");
    return CA_UNBOUND;
  }
  for (int i = 0; i < count && !rt->error; i++) {
    int kind = sqlite3_column_type(statement, i);
    if (kind == SQLITE_NULL)
      values[i] = CA_NONE;
    else if (kind == SQLITE_INTEGER)
      values[i] = ca_int(sqlite3_column_int64(statement, i));
    else if (kind == SQLITE_FLOAT)
      values[i] = ca_float(sqlite3_column_double(statement, i));
    else if (kind == SQLITE_BLOB)
      values[i] = ca_bytes(rt, sqlite3_column_blob(statement, i),
                           (size_t)sqlite3_column_bytes(statement, i));
    else {
      int length = sqlite3_column_bytes(statement, i);
      const unsigned char *text = sqlite3_column_text(statement, i);
      if (!text && length) {
        free(values);
        ca_error(rt, "MemoryError", "SQLite text allocation failed");
        return CA_UNBOUND;
      }
      CaValue bytes = ca_bytes(rt, (const char *)text, (size_t)length);
      values[i] = ca_text_call(rt, N_TEXT_DECODE, 1, 0, &bytes, NULL);
      if (rt->error) {
        ca_exception_take(rt);
        ca_error(rt, "sqlite3.OperationalError",
                 "Could not decode SQLite text to UTF-8");
      }
    }
  }
  if (!rt->error)
    result = ca_container(rt, CA_TUPLE_T, (size_t)count, values);
  free(values);
  return rt->error ? CA_UNBOUND : result;
}
static int bind_value(CaRuntime *rt, sqlite3_stmt *stmt, int index,
                      CaValue value) {
  int result = SQLITE_OK;
  switch (value.tag) {
  case CA_NONE_T:
    result = sqlite3_bind_null(stmt, index);
    break;
  case CA_BOOL_T:
  case CA_INT_T:
    result = sqlite3_bind_int64(stmt, index, value.as.i);
    break;
  case CA_BIG_T:
    ca_error(rt, "OverflowError",
             "Python int too large to convert to SQLite INTEGER");
    return 0;
  case CA_FLOAT_T:
    result = sqlite3_bind_double(stmt, index, value.as.f);
    break;
  case CA_STR_T: {
    CaString *s = (CaString *)value.as.obj;
    result = sqlite3_bind_text64(stmt, index, s->data, s->length,
                                 SQLITE_TRANSIENT, SQLITE_UTF8);
    break;
  }
  case CA_BYTES_T: {
    CaString *s = (CaString *)value.as.obj;
    result =
        sqlite3_bind_blob64(stmt, index, s->data, s->length, SQLITE_TRANSIENT);
    break;
  }
  default:
    ca_error(rt, "sqlite3.ProgrammingError",
             "unsupported SQLite parameter type");
    return 0;
  }
  if (result != SQLITE_OK) {
    fail(rt, sqlite3_db_handle(stmt), result);
    return 0;
  }
  return 1;
}
static int bind_all(CaRuntime *rt, sqlite3_stmt *stmt, CaValue parameters) {
  int n = sqlite3_bind_parameter_count(stmt);
  if (parameters.tag == CA_DICT_T) {
    for (int i = 1; i <= n; i++) {
      const char *name = sqlite3_bind_parameter_name(stmt, i);
      if (!name) {
        ca_error(rt, "sqlite3.ProgrammingError",
                 "Binding has no name, but a dictionary was supplied");
        return 0;
      }
      CaValue key = ca_string(rt, name + 1, strlen(name + 1));
      if (rt->error)
        return 0;
      CaValue value = ca_getitem(rt, parameters, key);
      if (rt->error) {
        ca_exception_take(rt);
        ca_error(rt, "sqlite3.ProgrammingError",
                 "Missing value for named SQL parameter");
        return 0;
      }
      if (!bind_value(rt, stmt, i, value))
        return 0;
    }
    return 1;
  }
  if (parameters.tag != CA_TUPLE_T && parameters.tag != CA_LIST_T) {
    ca_error(rt, "sqlite3.ProgrammingError",
             "parameters must be a sequence or dictionary");
    return 0;
  }
  CaContainer *values = (CaContainer *)parameters.as.obj;
  if (values->length != (size_t)n) {
    ca_error(rt, "sqlite3.ProgrammingError",
             "Incorrect number of bindings supplied");
    return 0;
  }
  for (int i = 0; i < n; i++)
    if (!bind_value(rt, stmt, i + 1, values->items[i]))
      return 0;
  return 1;
}
static int advance_cursor(CaRuntime *rt, CaForeign *foreign) {
  Cursor *cursor = foreign->handle;
  CaContainer *state = (CaContainer *)foreign->state.as.obj;
  if (cursor->done) {
    state->items[1] = CA_NONE;
    return 0;
  }
  sqlite3 *db = connection(rt, state->items[0]);
  if (!db)
    return -1;
  int status = sqlite3_step(cursor->statement);
  if (status == SQLITE_ROW) {
    state->items[1] = row(rt, cursor->statement);
    return rt->error ? -1 : 1;
  }
  cursor->done = 1;
  state->items[1] = CA_NONE;
  if (status != SQLITE_DONE) {
    fail(rt, db, status);
    return -1;
  }
  return 0;
}
int ca_sql_next(CaRuntime *rt, CaValue value, CaValue *out) {
  if (value.tag != CA_FOREIGN_T ||
      ((CaForeign *)value.as.obj)->kind != CA_F_SQL_CURSOR) {
    ca_error(rt, "TypeError", "expected native SQLite cursor");
    return -1;
  }
  CaForeign *f = (CaForeign *)value.as.obj;
  if (!f->handle) {
    ca_error(rt, "sqlite3.ProgrammingError",
             "Cannot operate on a closed cursor.");
    return -1;
  }
  CaContainer *state = (CaContainer *)f->state.as.obj;
  if (!connection(rt, state->items[0]))
    return -1;
  CaValue current = state->items[1];
  if (current.tag == CA_NONE_T) {
    *out = CA_UNBOUND;
    return 0;
  }
  /* Keep the returned row rooted while the next SQLite row is converted. */
  CaFrame frame;
  ca_push(rt, &frame, &current, 1);
  int status = advance_cursor(rt, f);
  ca_pop(rt, &frame);
  if (status < 0)
    return -1;
  *out = current;
  return 1;
}
void ca_sql_finalize(CaForeign *f) {
  if (f->kind == CA_F_SQL_CURSOR && f->handle) {
    Cursor *c = f->handle;
    if (c->statement)
      sqlite3_finalize(c->statement);
    free(c);
    f->handle = NULL;
  }
  if (f->kind == CA_F_SQL_CONNECTION && f->handle) {
    sqlite3_close_v2(f->handle);
    f->handle = NULL;
  }
}
CaValue ca_sql_call(CaRuntime *rt, int kind, int n, int nkw, CaValue *args,
                    const char **keys) {
  /* Reclaim unreachable statements before acquiring locks or committing. A
     tracing heap must not let a discarded PRAGMA cursor retain a write lock. */
  CaFrame argument_roots;
  ca_push(rt, &argument_roots, args, (size_t)(n + nkw));
  ca_collect(rt);
  ca_pop(rt, &argument_roots);
  CaValue out[8], defaults[] = {CA_NONE, CA_NONE, CA_NONE, CA_NONE,
                                CA_NONE, CA_NONE, CA_NONE, CA_NONE};
  if (kind == N_SQL_CONNECT) {
    const char *names[] = {
        "database",          "timeout", "detect_types",      "isolation_level",
        "check_same_thread", "factory", "cached_statements", "uri"};
    defaults[1] = ca_float(5);
    defaults[2] = ca_int(0);
    defaults[3] = ca_string(rt, "", 0);
    defaults[4] = ca_bool(1);
    defaults[6] = ca_int(128);
    defaults[7] = ca_bool(0);
    if (!ca_native_arguments(rt, n, nkw, args, keys, 8, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    const char *name = ca_native_text(rt, out[0], 1);
    if (!name)
      return CA_UNBOUND;
    double timeout = ca_as_double(rt, out[1]);
    if (rt->error)
      return CA_UNBOUND;
    if (out[2].tag != CA_INT_T || out[2].as.i || out[5].tag != CA_NONE_T) {
      ca_error(
          rt, "NotImplementedError",
          "SQLite type converters and custom factories are not implemented");
      return CA_UNBOUND;
    }
    if (out[3].tag != CA_NONE_T) {
      const char *isolation = ca_native_text(rt, out[3], 0);
      if (!isolation)
        return CA_UNBOUND;
      if (strcmp(isolation, "") && strcasecmp(isolation, "DEFERRED") &&
          strcasecmp(isolation, "IMMEDIATE") &&
          strcasecmp(isolation, "EXCLUSIVE")) {
        ca_error(rt, "ValueError", "invalid isolation_level");
        return CA_UNBOUND;
      }
    }
    if (!isfinite(timeout) || timeout > (double)INT_MAX / 1000) {
      ca_error(rt, "OverflowError", "SQLite timeout out of range");
      return CA_UNBOUND;
    }
    sqlite3 *db = NULL;
    int status = sqlite3_open_v2(
        name, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX |
            (ca_truth(rt, out[7]) ? SQLITE_OPEN_URI : 0),
        NULL);
    if (status != SQLITE_OK) {
      CaValue result = fail(rt, db, status);
      if (db)
        sqlite3_close_v2(db);
      return result;
    }
    sqlite3_extended_result_codes(db, 1);
    sqlite3_busy_timeout(db, timeout < 0 ? 0 : (int)(timeout * 1000));
    CaValue result = ca_native_object(rt, CA_F_SQL_CONNECTION, out[3], db, 0);
    if (rt->error)
      sqlite3_close_v2(db);
    return result;
  }
  if (kind == N_SQL_EXECUTE) {
    const char *names[] = {"self", "sql", "parameters"};
    defaults[2] = ca_container(rt, CA_TUPLE_T, 0, NULL);
    if (!ca_native_arguments(rt, n, nkw, args, keys, 3, 2, names, defaults,
                             out))
      return CA_UNBOUND;
    CaValue owner = out[0];
    CaForeign *reuse = NULL;
    if (owner.tag == CA_FOREIGN_T &&
        ((CaForeign *)owner.as.obj)->kind == CA_F_SQL_CURSOR) {
      reuse = (CaForeign *)owner.as.obj;
      if (!reuse->handle) {
        ca_error(rt, "sqlite3.ProgrammingError",
                 "Cannot operate on a closed cursor.");
        return CA_UNBOUND;
      }
      owner = ca_capture(reuse->state, 0);
    }
    sqlite3 *db = connection(rt, owner);
    if (!db)
      return CA_UNBOUND;
    const char *sql = ca_native_text(rt, out[1], 0);
    if (!sql)
      return CA_UNBOUND;
    sqlite3_stmt *statement = NULL;
    const char *tail = NULL;
    int status = sqlite3_prepare_v2(db, sql, -1, &statement, &tail);
    if (status != SQLITE_OK)
      return fail(rt, db, status);
    while (tail && isspace((unsigned char)*tail))
      tail++;
    if (tail && *tail) {
      sqlite3_stmt *extra = NULL;
      const char *end = NULL;
      status = sqlite3_prepare_v2(db, tail, -1, &extra, &end);
      if (extra)
        sqlite3_finalize(extra);
      if (extra || status != SQLITE_OK) {
        if (statement)
          sqlite3_finalize(statement);
        ca_error(rt, "sqlite3.ProgrammingError",
                 "You can only execute one statement at a time.");
        return CA_UNBOUND;
      }
    }
    if (statement && !bind_all(rt, statement, out[2])) {
      sqlite3_finalize(statement);
      return CA_UNBOUND;
    }
    int changes = dml(sql);
    CaForeign *connection_value = (CaForeign *)owner.as.obj;
    if (changes && connection_value->state.tag != CA_NONE_T &&
        sqlite3_get_autocommit(db)) {
      const char *isolation = ca_native_text(rt, connection_value->state, 0);
      if (!isolation) {
        sqlite3_finalize(statement);
        return CA_UNBOUND;
      }
      char begin[32];
      snprintf(begin, sizeof begin, "BEGIN %s", isolation);
      status = sqlite3_exec(db, begin, NULL, NULL, NULL);
      if (status != SQLITE_OK) {
        CaValue failure = fail(rt, db, status);
        sqlite3_finalize(statement);
        return failure;
      }
    }
    Cursor *cursor = calloc(1, sizeof *cursor);
    if (!cursor) {
      if (statement)
        sqlite3_finalize(statement);
      ca_error(rt, "MemoryError", "SQLite cursor allocation failed");
      return CA_UNBOUND;
    }
    cursor->statement = statement;
    cursor->done = !statement;
    CaValue values[] = {owner, CA_NONE};
    CaValue state = ca_container(rt, CA_TUPLE_T, 2, values);
    if (rt->error) {
      if (statement)
        sqlite3_finalize(statement);
      free(cursor);
      return CA_UNBOUND;
    }
    CaValue result;
    if (reuse) {
      Cursor *old = reuse->handle;
      if (old->statement)
        sqlite3_finalize(old->statement);
      free(old);
      reuse->handle = cursor;
      reuse->state = state;
      reuse->auxiliary = -1;
      result = out[0];
    } else
      result = ca_native_object(rt, CA_F_SQL_CURSOR, state, cursor, -1);
    if (rt->error) {
      if (statement)
        sqlite3_finalize(statement);
      free(cursor);
      return CA_UNBOUND;
    }
    CaForeign *foreign = (CaForeign *)result.as.obj;
    if (statement && advance_cursor(rt, foreign) < 0)
      return CA_UNBOUND;
    if (changes && cursor->done)
      foreign->auxiliary = sqlite3_changes(db);
    return result;
  }
  const char *names[] = {"self", "kind", "exception", "traceback"};
  int total = kind == N_SQL_EXIT ? 4 : 1;
  if (!ca_native_arguments(rt, n, nkw, args, keys, total, 1, names, defaults,
                           out))
    return CA_UNBOUND;
  if (kind == N_SQL_CLOSE) {
    if (out[0].tag != CA_FOREIGN_T) {
      ca_error(rt, "TypeError", "expected SQLite object");
      return CA_UNBOUND;
    }
    ca_sql_finalize((CaForeign *)out[0].as.obj);
    return CA_NONE;
  }
  if (kind == N_SQL_FETCHONE) {
    CaValue row_value = CA_UNBOUND;
    int status = ca_sql_next(rt, out[0], &row_value);
    return status < 0 ? CA_UNBOUND : status ? row_value : CA_NONE;
  }
  if (kind == N_SQL_FETCHALL) {
    CaValue roots[] = {out[0], ca_container(rt, CA_LIST_T, 0, NULL),
                       CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 3);
    if (!rt->error)
      while (ca_sql_next(rt, roots[0], &roots[2]) > 0 && !rt->error) {
        ca_append(rt, roots[1], roots[2]);
        if (rt->error)
          break;
        ca_safepoint(rt);
      }
    CaValue result = roots[1];
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : result;
  }
  sqlite3 *db = connection(rt, out[0]);
  if (!db)
    return CA_UNBOUND;
  if (kind == N_SQL_ENTER)
    return out[0];
  if (kind == N_SQL_COMMIT || kind == N_SQL_ROLLBACK || kind == N_SQL_EXIT) {
    if (!sqlite3_get_autocommit(db)) {
      int rollback = kind == N_SQL_ROLLBACK ||
                     (kind == N_SQL_EXIT && out[1].tag != CA_NONE_T);
      int status =
          sqlite3_exec(db, rollback ? "ROLLBACK" : "COMMIT", NULL, NULL, NULL);
      if (status != SQLITE_OK) {
        CaValue failure = fail(rt, db, status);
        if (!rollback)
          sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return failure;
      }
    }
    return kind == N_SQL_EXIT ? ca_bool(0) : CA_NONE;
  }
  ca_error(rt, "NotImplementedError", "SQLite API overload is not implemented");
  return CA_UNBOUND;
}
#else
CaValue ca_sql_call(CaRuntime *rt, int k, int n, int nk, CaValue *a,
                    const char **keys) {
  (void)k;
  (void)n;
  (void)nk;
  (void)a;
  (void)keys;
  ca_error(rt, "ImportError", "SQLite support was not linked");
  return CA_UNBOUND;
}
void ca_sql_finalize(CaForeign *f) { (void)f; }
int ca_sql_next(CaRuntime *rt, CaValue v, CaValue *out) {
  (void)v;
  (void)out;
  ca_error(rt, "TypeError", "value is not an iterator");
  return -1;
}
#endif
