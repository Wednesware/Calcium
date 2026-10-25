#ifndef CALCIUM_STANDARD_H
#define CALCIUM_STANDARD_H
#include "cart.h"
enum {
  CA_F_STATIC = 1,
  CA_F_CLASSMETHOD,
  CA_F_PROPERTY,
  CA_F_PATH = 10,
  CA_F_FILE,
  CA_F_HASH,
  CA_F_SQL_CONNECTION,
  CA_F_SQL_CURSOR,
  CA_F_FIELD,
  CA_F_DATETIME,
  CA_F_UTC,
  CA_F_UUID,
  CA_F_ENVIRON,
  CA_F_PARTIAL,
  CA_F_DIRECTORY_ITER,
  CA_F_ENUMERATE,
  CA_F_ZIP,
  CA_F_DICT_VIEW,
  CA_F_STAT,
  CA_F_DICT_ITER
};
enum { CA_T_PATH = 300, CA_T_DATETIME, CA_T_TIMEZONE, CA_T_UUID };
enum {
  N_TIME = 1000,
  N_MONOTONIC,
  N_SLEEP,
  N_GETCWD,
  N_GETENV,
  N_REPLACE,
  N_UNLINK,
  N_FSYNC,
  N_FDOPEN,
  N_PATH_EXISTS,
  N_PATH_ISFILE,
  N_PATH_ISDIR,
  N_PATH_JOIN,
  N_PATH_ABSPATH,
  N_PATH_DIRNAME,
  N_PATH_BASENAME,
  N_PATH_HOME,
  N_PATH_RESOLVE,
  N_PATH_EXPANDUSER,
  N_PATH_MKDIR,
  N_PATH_READTEXT,
  N_PATH_WRITETEXT,
  N_PATH_READBYTES,
  N_PATH_WRITEBYTES,
  N_PATH_IS_FILE,
  N_PATH_IS_DIR,
  N_PATH_EXISTS_METHOD,
  N_PATH_STAT,
  N_PATH_RELATIVE,
  N_PATH_AS_POSIX,
  N_PATH_RGLOB,
  N_PATH_ITERDIR,
  N_FILE_READ,
  N_FILE_WRITE,
  N_FILE_FLUSH,
  N_FILE_FILENO,
  N_FILE_CLOSE,
  N_FILE_ENTER,
  N_FILE_EXIT,
  N_MKSTEMP,
  N_GETTEMPDIR,
  N_COPYTREE,
  N_RMTREE,
  N_SHA256,
  N_HASH_UPDATE,
  N_HASH_DIGEST,
  N_HASH_HEXDIGEST,
  N_HASH_COPY,
  N_JSON_LOADS,
  N_JSON_DUMPS,
  N_UUID4,
  N_TOKEN_HEX,
  N_TOKEN_URLSAFE,
  N_DATETIME_NOW,
  N_DATETIME_FROMTIMESTAMP,
  N_DATETIME_ISOFORMAT,
  N_DATACLASS,
  N_FIELD,
  N_ASDICT,
  N_PARTIAL,
  N_SQL_CONNECT,
  N_SQL_EXECUTE,
  N_SQL_FETCHONE,
  N_SQL_FETCHALL,
  N_SQL_COMMIT,
  N_SQL_ROLLBACK,
  N_SQL_CLOSE,
  N_SQL_ENTER,
  N_SQL_EXIT,
  N_ENVIRON_GET,
  N_TEXT_ENCODE,
  N_TEXT_DECODE,
  N_TEXT_JOIN,
  N_TEXT_REPLACE,
  N_TEXT_LOWER,
  N_TEXT_UPPER,
  N_TEXT_STRIP,
  N_TEXT_LSTRIP,
  N_TEXT_RSTRIP,
  N_TEXT_STARTSWITH,
  N_TEXT_ENDSWITH,
  N_TEXT_SPLIT,
  N_TEXT_RSplit,
  N_TEXT_FIND,
  N_TEXT_COUNT,
  N_DICT_ITEMS,
  N_DICT_KEYS,
  N_DICT_VALUES,
  N_DICT_FROMKEYS,
  N_PARTIAL_INVOKE,
  N_GEN_SEND,
  N_GEN_CLOSE,
  N_GEN_THROW,
  N_LAST
};
CaValue ca_native_function(int);
CaValue ca_native_method(CaRuntime *, CaValue, int);
CaValue ca_native_object(CaRuntime *, int, CaValue, void *, long);
const char *ca_native_text(CaRuntime *, CaValue, int);
int ca_native_arguments(CaRuntime *, int, int, CaValue *, const char **, int,
                        int, const char **, CaValue *, CaValue *);
CaValue ca_native_oserror(CaRuntime *, int, const char *);
CaValue ca_path_new(CaRuntime *, const char *);
CaValue ca_io_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_data_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_system_attribute(CaRuntime *, CaValue, const char *);
CaValue ca_system_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_sql_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_text_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_path_attribute(CaRuntime *, CaValue, const char *);
CaValue ca_data_attribute(CaRuntime *, CaValue, const char *);
void ca_io_finalize(CaForeign *);
void ca_data_finalize(CaForeign *);
void ca_sql_finalize(CaForeign *);
int ca_io_next(CaRuntime *, CaValue, CaValue *);
int ca_sql_next(CaRuntime *, CaValue, CaValue *);
const char *ca_standard_type_name(long);
CaValue ca_namespace_get(CaValue, const char *);
void ca_namespace_set(CaRuntime *, CaValue, const char *, CaValue);
CaValue ca_class_lookup(CaValue, const char *);
int ca_dataclass_repr_field(CaValue, const char *);
int ca_dataclass_equal_field(CaValue, const char *);
CaValue ca_json_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_dataclass_construct(CaRuntime *, CaValue, int, int, CaValue *,
                               const char **);
#endif
