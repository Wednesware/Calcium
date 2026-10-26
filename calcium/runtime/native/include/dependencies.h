#ifndef CALCIUM_DEPENDENCIES_H
#define CALCIUM_DEPENDENCIES_H
#include "standard.h"
#define CA_DEPENDENCY_FUNCTIONS(X)                                             \
  X(D_RE_COMPILE, "re_compile")                                                \
  X(D_RE_MATCH, "re_match")                                                    \
  X(D_RE_SEARCH, "re_search")                                                  \
  X(D_RE_FULLMATCH, "re_fullmatch")                                            \
  X(D_RE_FINDALL, "re_findall")                                                \
  X(D_RE_FINDITER, "re_finditer")                                              \
  X(D_RE_SPLIT, "re_split")                                                    \
  X(D_RE_SUB, "re_sub")                                                        \
  X(D_RE_SUBN, "re_subn")                                                      \
  X(D_RE_ESCAPE, "re_escape")                                                  \
  X(D_RE_GROUP, "re_group")                                                    \
  X(D_RE_GROUPS, "re_groups")                                                  \
  X(D_RE_GROUPDICT, "re_groupdict")                                            \
  X(D_RE_START, "re_start")                                                    \
  X(D_RE_END, "re_end")                                                        \
  X(D_RE_SPAN, "re_span")                                                      \
  X(D_RE_EXPAND, "re_expand")                                                  \
  X(D_PROCESS_RUN, "process_run")                                              \
  X(D_PROCESS_POPEN, "process_popen")                                          \
  X(D_PROCESS_WAIT, "process_wait")                                            \
  X(D_PROCESS_POLL, "process_poll")                                            \
  X(D_PROCESS_COMMUNICATE, "process_communicate")                              \
  X(D_PROCESS_TERMINATE, "process_terminate")                                  \
  X(D_PROCESS_KILL, "process_kill")                                            \
  X(D_PROCESS_CHECK_RETURN, "process_check_return")                            \
  X(D_PROCESS_CHECK_CALL, "process_check_call")                                \
  X(D_PROCESS_CHECK_OUTPUT, "process_check_output")                            \
  X(D_ARCHIVE_ZIPFILE, "archive_zipfile")                                      \
  X(D_ARCHIVE_READ, "archive_read")                                            \
  X(D_ARCHIVE_NAMELIST, "archive_namelist")                                    \
  X(D_ARCHIVE_INFOLIST, "archive_infolist")                                    \
  X(D_ARCHIVE_GETINFO, "archive_getinfo")                                      \
  X(D_ARCHIVE_EXTRACT, "archive_extract")                                      \
  X(D_ARCHIVE_EXTRACTALL, "archive_extractall")                                \
  X(D_ARCHIVE_WRITE, "archive_write")                                          \
  X(D_ARCHIVE_WRITESTR, "archive_writestr")                                    \
  X(D_ARCHIVE_CLOSE, "archive_close")                                          \
  X(D_ARCHIVE_ENTER, "archive_enter")                                          \
  X(D_ARCHIVE_EXIT, "archive_exit")                                            \
  X(D_ARCHIVE_IS_ZIPFILE, "archive_is_zipfile")                                \
  X(D_ARCHIVE_TESTZIP, "archive_testzip")                                      \
  X(D_URL_RETRIEVE, "url_retrieve")                                            \
  X(D_URL_OPEN, "url_open")                                                    \
  X(D_URL_READ, "url_read")                                                    \
  X(D_URL_CLOSE, "url_close")                                                  \
  X(D_URL_ENTER, "url_enter")                                                  \
  X(D_URL_EXIT, "url_exit")                                                    \
  X(D_URL_GETHEADER, "url_getheader")                                          \
  X(D_URL_GETCODE, "url_getcode")                                              \
  X(D_URL_GETURL, "url_geturl")                                                \
  X(D_IMPORT_MODULE, "import_module")                                          \
  X(D_IMPORT_FIND_SPEC, "import_find_spec")                                    \
  X(D_IMPORT_SPEC_FILE, "import_spec_file")                                    \
  X(D_IMPORT_MODULE_SPEC, "import_module_spec")                                \
  X(D_IMPORT_EXEC_MODULE, "import_exec_module")                                \
  X(D_IMPORT_INVALIDATE, "import_invalidate")                                  \
  X(D_TRACE_FORMAT_EXC, "trace_format_exc")                                    \
  X(D_TRACE_FORMAT_EXCEPTION, "trace_format_exception")                        \
  X(D_TRACE_FORMAT_ONLY, "trace_format_only")                                  \
  X(D_TRACE_PRINT_EXC, "trace_print_exc")                                      \
  X(D_TRACE_PRINT_EXCEPTION, "trace_print_exception")                          \
  X(D_THREAD_THREAD, "thread_thread")                                          \
  X(D_THREAD_START, "thread_start")                                            \
  X(D_THREAD_JOIN, "thread_join")                                              \
  X(D_THREAD_ALIVE, "thread_alive")                                            \
  X(D_THREAD_RLOCK, "thread_rlock")                                            \
  X(D_THREAD_LOCK, "thread_lock")                                              \
  X(D_THREAD_ACQUIRE, "thread_acquire")                                        \
  X(D_THREAD_RELEASE, "thread_release")                                        \
  X(D_THREAD_LOCKED, "thread_locked")                                          \
  X(D_THREAD_ENTER, "thread_enter")                                            \
  X(D_THREAD_EXIT, "thread_exit")                                              \
  X(D_THREAD_EVENT, "thread_event")                                            \
  X(D_THREAD_SET, "thread_set")                                                \
  X(D_THREAD_CLEAR, "thread_clear")                                            \
  X(D_THREAD_IS_SET, "thread_is_set")                                          \
  X(D_THREAD_WAIT, "thread_wait")                                              \
  X(D_THREAD_IDENT, "thread_ident")                                            \
  X(D_THREAD_CURRENT, "thread_current")                                        \
  X(D_THREAD_ACTIVE, "thread_active")                                          \
  X(D_HELPERS_RUNNING_LOOP, "helpers_running_loop")                            \
  X(D_HELPERS_LOOP_RUNNING, "helpers_loop_running")                            \
  X(D_HELPERS_JSON_LOAD, "helpers_json_load")                                  \
  X(D_HELPERS_JSON_DUMP, "helpers_json_dump")
enum {
  D_BASE = 1999,
#define DEP_ID(id, name) id,
  CA_DEPENDENCY_FUNCTIONS(DEP_ID)
#undef DEP_ID
      D_LAST
};
enum {
  CA_F_REGEX = 100,
  CA_F_MATCH,
  CA_F_REGEX_ITER,
  CA_F_PROCESS,
  CA_F_ZIPFILE,
  CA_F_URL,
  CA_F_THREAD,
  CA_F_THREAD_LOCK,
  CA_F_EVENT
};
void ca_dependency_exception(CaRuntime *, CaValue, int, CaValue *);
void ca_dependency_exception_message(CaRuntime *, CaValue, int, CaValue *);
CaValue ca_trace_capture(CaRuntime *);
CaValue ca_async_running_loop(CaRuntime *);
int ca_dependency_available(const char *);
CaValue ca_dependency_function(int);
CaValue ca_dependency_call(CaRuntime *, int, int, int, CaValue *,
                           const char **);
CaValue ca_dependency_init(CaRuntime *, int, CaValue);
CaValue ca_dependency_attribute(CaRuntime *, CaValue, const char *);
void ca_dependency_set(CaRuntime *, CaValue, const char *, CaValue);
void ca_dependency_finalize(CaForeign *);
CaValue ca_dep_get(CaValue, const char *);
void ca_dep_put(CaRuntime *, CaValue, const char *, CaValue);
CaValue ca_dep_record(CaRuntime *, const char *);
CaValue ca_dep_error(CaRuntime *, const char *, const char *);
CaValue ca_re_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_process_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_archive_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_url_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_import_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_trace_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_thread_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_re_attribute(CaRuntime *, CaValue, const char *);
void ca_re_finalize(CaForeign *);
int ca_re_next(CaRuntime *, CaValue, CaValue *);
CaValue ca_process_attribute(CaRuntime *, CaValue, const char *);
void ca_process_finalize(CaForeign *);
CaValue ca_archive_attribute(CaRuntime *, CaValue, const char *);
void ca_archive_finalize(CaForeign *);
CaValue ca_url_attribute(CaRuntime *, CaValue, const char *);
CaValue ca_thread_attribute(CaRuntime *, CaValue, const char *);
void ca_thread_finalize(CaForeign *);
void ca_thread_release(CaRuntime *);
void ca_thread_acquire(CaRuntime *);
void ca_thread_yield(CaRuntime *);
int ca_thread_shutdown(CaRuntime *);
void ca_thread_roots(CaRuntime *, void (*)(CaValue, CaObj **), CaObj **);
CaModule *ca_import_registry(CaRuntime *, const char *);
#endif
