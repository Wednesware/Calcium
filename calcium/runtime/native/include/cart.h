#ifndef CALCIUM_CART_H
#define CALCIUM_CART_H
#ifndef CART_PY_MINOR
#define CART_PY_MINOR 14
#endif
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* CaRT ABI 5. Tagged primitives, module namespaces and traced heap objects,
 * tracing roots. */
typedef struct CaObj CaObj;
typedef struct CaRuntime CaRuntime;
typedef struct CaFunction CaFunction;
typedef struct CaModule CaModule;
typedef struct CaBinding CaBinding;
typedef enum {
  CA_UNBOUND_T,
  CA_NONE_T,
  CA_BOOL_T,
  CA_INT_T,
  CA_BIG_T,
  CA_FLOAT_T,
  CA_STR_T,
  CA_FUNCTION_T,
  CA_RANGE_T,
  CA_ITER_T,
  CA_MODULE_T,
  CA_LIST_T,
  CA_TUPLE_T,
  CA_DICT_T,
  CA_SET_T,
  CA_SLICE_T,
  CA_METHOD_T,
  CA_CELL_T,
  CA_CLOSURE_T,
  CA_CLASS_T,
  CA_INSTANCE_T,
  CA_BOUND_T,
  CA_TYPE_T,
  CA_EXCEPTION_T,
  CA_BYTES_T,
  CA_FOREIGN_T,
  CA_GENERATOR_T
} CaTag;
typedef struct {
  CaTag tag;
  uint64_t identity; /* preserves NaN identity across tagged-value copies */
  union {
    int64_t i;
    double f;
    CaObj *obj;
    const CaFunction *fn;
    CaModule *module;
  } as;
} CaValue;
#define CA_UNBOUND ((CaValue){.tag = CA_UNBOUND_T})
#define CA_NONE ((CaValue){.tag = CA_NONE_T})
uint64_t ca_next_identity(void);
static inline CaValue ca_int(int64_t i) {
  return (CaValue){.tag = CA_INT_T,
                   .identity = (i >= -5 && i <= 256) ? 0 : ca_next_identity(),
                   .as.i = i};
}
static inline CaValue ca_bool(int i) {
  return (CaValue){.tag = CA_BOOL_T, .as.i = !!i};
}
CaValue ca_float(double);
static inline CaValue ca_function(const CaFunction *f) {
  return (CaValue){.tag = CA_FUNCTION_T, .as.fn = f};
}
typedef struct CaFrame {
  struct CaFrame *previous;
  CaValue *values;
  size_t count;
} CaFrame;
struct CaObj {
  CaObj *next, *mark_next;
  size_t bytes;
  int marked;
  CaTag kind;
};
struct CaRuntime {
  CaObj *heap;
  CaFrame *roots;
  size_t bytes, threshold, collections, allocations, peak_bytes;
  unsigned depth;
  CaModule **modules;
  size_t module_count;
  CaValue environment, pending, handled;
  CaValue arguments;
  void *event_loop;
  CaModule **standard_modules;
  size_t standard_count;
  const char *error;
  char message[256];
};
typedef CaValue (*CaCode)(CaRuntime *, CaValue *);
struct CaFunction {
  const char *name;
  int count, posonly;
  const char **parameters;
  CaCode code;
  int builtin;
  int positional;
  CaValue *defaults;
  int vararg, kwarg;
};
typedef struct {
  CaObj base;
  int sign;
  size_t count;
  uint32_t limbs[];
} CaBig;
typedef struct {
  CaObj base;
  size_t length;
  char data[];
} CaString;
typedef struct {
  CaObj base;
  CaValue start, stop, step;
} CaRange;
typedef struct {
  CaObj base;
  CaValue iterable, current;
  size_t index;
} CaIter;
struct CaBinding {
  CaBinding *next;
  char *name;
  CaValue value;
};
struct CaModule {
  const char *name, *package;
  int is_package, state; /* 0 absent, 1 initializing, 2 loaded */
  const char **names;
  CaValue *values;
  size_t count;
  CaValue (*initialize)(CaRuntime *);
  CaBinding *extra;
};
static inline CaValue ca_module_value(CaModule *m) {
  return (CaValue){.tag = CA_MODULE_T, .as.module = m};
}
CaValue ca_import(CaRuntime *, CaModule *, const char *, int, int,
                  const char **);
CaValue ca_attribute(CaRuntime *, CaValue, const char *, int);
void ca_set_attribute(CaRuntime *, CaValue, const char *, CaValue);
CaValue ca_module_start(CaRuntime *, CaModule *);
void ca_modules_destroy(CaRuntime *);
typedef struct {
  CaObj base;
  size_t length, capacity;
  CaValue *items;
  int printing;
} CaContainer;
typedef struct {
  CaObj base;
  CaValue start, stop, step;
} CaSlice;
typedef struct {
  CaObj base;
  CaValue receiver;
  int method;
} CaMethod;
int ca_is_container(CaValue);
CaValue ca_container(CaRuntime *, CaTag, size_t, CaValue *);
void ca_append(CaRuntime *, CaValue, CaValue);
CaValue ca_getitem(CaRuntime *, CaValue, CaValue);
void ca_setitem(CaRuntime *, CaValue, CaValue, CaValue);
CaValue ca_slice(CaRuntime *, CaValue, CaValue, CaValue);
CaValue ca_container_compare(CaRuntime *, CaValue, CaValue, const char *);
int ca_contains(CaRuntime *, CaValue, CaValue);
void ca_print_value(CaRuntime *, CaValue);
void ca_print_container(CaRuntime *, CaValue);
void ca_repr_string(CaString *);
CaValue ca_sequence_binary(CaRuntime *, CaValue, CaValue, char);
CaValue ca_collection_builtin(CaRuntime *, int, int, int, CaValue *);
void ca_extend(CaRuntime *, CaValue, CaValue);
void ca_kwargs_merge(CaRuntime *, CaValue, CaValue, int);
void ca_kwargs_set(CaRuntime *, CaValue, const char *, CaValue);
CaValue ca_call_ex(CaRuntime *, CaValue, CaValue, CaValue);
CaValue ca_bound_method(CaRuntime *, CaValue, const char *);
CaValue ca_method_call(CaRuntime *, CaValue, int, int, CaValue *);
CaValue ca_unpack(CaRuntime *, CaValue, size_t);
CaValue ca_dict_constructor(CaRuntime *, int, int, CaValue *, const char **);
typedef struct CaGenerator CaGenerator;
typedef int (*CaResume)(CaRuntime *, CaGenerator *, CaValue *);
struct CaGenerator {
  CaObj base;
  CaResume resume;
  const CaFunction *definition;
  size_t count;
  int pc, closed, running, started;
  int coroutine, claimed;
  CaValue result, sent, injected, handled;
  CaValue values[];
};
CaValue ca_generator_new(CaRuntime *, CaResume, const CaFunction *, size_t);
int ca_generator_next(CaRuntime *, CaValue, CaValue *);
CaValue ca_generator_call(CaRuntime *, int, int, int, CaValue *, const char **);
int ca_exit_status(CaRuntime *);
/* Internal object/closure ABI. */
typedef struct {
  CaObj base;
  CaValue value;
} CaCell;
typedef struct {
  CaObj base;
  CaFunction fn;
  CaValue environment;
  CaValue defaults[];
} CaClosure;
typedef struct {
  CaObj base;
  CaValue name, namespace_, parent, fields;
  int dataclass;
} CaClass;
typedef struct {
  CaObj base;
  CaValue type, namespace_;
  int printing;
} CaInstance;
typedef struct {
  CaObj base;
  CaValue receiver, function;
} CaBound;
typedef struct {
  CaObj base;
  CaValue type, args, cause, context, message;
} CaException;
typedef struct {
  CaObj base;
  int kind;
  void *handle;
  CaValue state;
  long auxiliary;
} CaForeign;
enum {
  CA_T_OBJECT = 100,
  CA_T_STR,
  CA_T_INT,
  CA_T_FLOAT,
  CA_T_BOOL,
  CA_T_BYTES,
  CA_T_NONE,
  CA_T_FUNCTION,
  CA_T_BUILTIN_FUNCTION,
  CA_T_METHOD,
  CA_T_TYPE,
  CA_T_LIST,
  CA_T_TUPLE,
  CA_T_DICT,
  CA_T_SET,
  CA_T_RANGE,
  CA_T_SLICE,
  CA_T_MAP,
  CA_T_FILTER,
  CA_T_REVERSED,
  CA_T_LIST_REVERSE,
  CA_T_DICT_REVERSE,
  CA_T_BASEEXCEPTION = 200,
  CA_T_EXCEPTION,
  CA_T_TYPEERROR,
  CA_T_VALUEERROR,
  CA_T_RUNTIMEERROR,
  CA_T_NAMEERROR,
  CA_T_UNBOUNDLOCALERROR,
  CA_T_ATTRIBUTEERROR,
  CA_T_LOOKUPERROR,
  CA_T_KEYERROR,
  CA_T_INDEXERROR,
  CA_T_OSERROR,
  CA_T_FILENOTFOUNDERROR,
  CA_T_FILEEXISTSERROR,
  CA_T_PERMISSIONERROR,
  CA_T_IMPORTERROR,
  CA_T_MODULENOTFOUNDERROR,
  CA_T_ZERODIVISIONERROR,
  CA_T_OVERFLOWERROR,
  CA_T_MEMORYERROR,
  CA_T_RECURSIONERROR,
  CA_T_STOPITERATION,
  CA_T_ASSERTIONERROR,
  CA_T_NOTIMPLEMENTEDERROR,
  CA_T_SYSTEMEXIT,
  CA_T_KEYBOARDINTERRUPT,
  CA_T_JSONDECODEERROR,
  CA_T_SQL_ERROR,
  CA_T_SQL_DATABASEERROR,
  CA_T_SQL_OPERATIONALERROR,
  CA_T_SQL_INTEGRITYERROR,
  CA_T_SQL_PROGRAMMINGERROR,
  CA_T_SQL_INTERFACEERROR,
  CA_T_SQL_DATAERROR,
  CA_T_UNICODEERROR,
  CA_T_UNICODEENCODEERROR,
  CA_T_UNICODEDECODEERROR,
  CA_T_CANCELLEDERROR,
  CA_T_TIMEOUTERROR,
  CA_T_ARITHMETICERROR,
  CA_T_FLOATINGPOINTERROR,
  CA_T_SYNTAXERROR,
  CA_T_INDENTATIONERROR,
  CA_T_TABERROR,
  CA_T_CONNECTIONERROR,
  CA_T_BROKENPIPEERROR,
  CA_T_CONNECTIONABORTEDERROR,
  CA_T_CONNECTIONREFUSEDERROR,
  CA_T_CONNECTIONRESETERROR,
  CA_T_EOFERROR,
  CA_T_BUFFERERROR,
  CA_T_GENERATOREXIT,
  CA_T_STOPASYNCITERATION,
  CA_T_BLOCKINGIOERROR,
  CA_T_CHILDPROCESSERROR,
  CA_T_INTERRUPTEDERROR,
  CA_T_ISADIRECTORYERROR,
  CA_T_NOTADIRECTORYERROR,
  CA_T_PROCESSLOOKUPERROR,
  CA_T_UNICODETRANSLATEERROR,
  CA_T_SYSTEMERROR,
  CA_T_REFERENCEERROR,
  CA_T_PYTHONFINALIZATIONERROR,
  CA_T_WARNING,
  CA_T_USERWARNING,
  CA_T_DEPRECATIONWARNING,
  CA_T_PENDINGDEPRECATIONWARNING,
  CA_T_SYNTAXWARNING,
  CA_T_RUNTIMEWARNING,
  CA_T_FUTUREWARNING,
  CA_T_IMPORTWARNING,
  CA_T_UNICODEWARNING,
  CA_T_BYTESWARNING,
  CA_T_RESOURCEWARNING,
  CA_T_ENCODINGWARNING,
};
static inline CaValue ca_type(long id) {
  return (CaValue){.tag = CA_TYPE_T, .as.i = id};
}
CaValue ca_cell(CaRuntime *, CaValue);
CaValue ca_cell_get(CaValue);
void ca_cell_set(CaValue, CaValue);
CaValue ca_capture(CaValue, size_t);
CaValue ca_closure(CaRuntime *, const CaFunction *, CaValue *, size_t,
                   CaValue *);
CaValue ca_class(CaRuntime *, const char *, size_t, CaValue *);
CaValue ca_class_finalize(CaRuntime *, CaValue);
CaValue ca_class_load(CaRuntime *, CaValue, CaValue, const char *);
CaValue ca_object_attribute(CaRuntime *, CaValue, const char *);
void ca_object_set(CaRuntime *, CaValue, const char *, CaValue);
CaValue ca_bound(CaRuntime *, CaValue, CaValue);
CaValue ca_object_compare(CaRuntime *, CaValue, CaValue, const char *, int *);
CaValue ca_type_of(CaValue);
CaValue ca_object_call(CaRuntime *, CaValue, int, int, CaValue *,
                       const char **);
int ca_subclass(CaRuntime *, CaValue, CaValue);
int ca_isinstance(CaRuntime *, CaValue, CaValue);
CaValue ca_builtin_more_lookup(const char *);
CaValue ca_builtin_more(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_builtin_index(CaRuntime *, CaValue);
int ca_file_close(CaForeign *);
CaValue ca_file_read(CaRuntime *, CaValue, long, int);
CaValue ca_file_builtin(CaRuntime *, int, int, int, CaValue *, const char **);
CaValue ca_more_method(CaRuntime *, CaValue, int);
CaValue ca_file_decode(CaRuntime *, CaForeign *, CaValue);
CaValue ca_file_option(CaForeign *, size_t);
CaValue ca_builtin_open(CaRuntime *, int, int, CaValue *, const char **);
int ca_builtin_iterator_next(CaRuntime *, CaValue, CaValue *);
CaValue ca_unpack_star(CaRuntime *, CaValue, size_t, size_t);
int ca_same_identity(CaValue, CaValue);
CaValue ca_identity(CaRuntime *, CaValue);
CaValue ca_integer_base(CaRuntime *, CaValue, int);
CaValue ca_hash_value(CaRuntime *, CaValue);
void ca_object_delete(CaRuntime *, CaValue, const char *);
CaValue ca_builtin_lookup(const char *);
const char *ca_type_name(CaValue);
CaValue ca_exception_type(const char *);
CaValue ca_exception_new(CaRuntime *, CaValue, int, CaValue *);
CaValue ca_exception_take(CaRuntime *);
CaValue ca_error_get(CaRuntime *);
void ca_error_set(CaRuntime *, CaValue);
void ca_raise(CaRuntime *, CaValue, CaValue, int);
int ca_exception_match(CaRuntime *, CaValue, CaValue);
CaValue ca_foreign_iter(CaRuntime *, CaValue);
int ca_foreign_next(CaRuntime *, CaValue, CaValue *);
void ca_foreign_render(CaRuntime *, FILE *, CaValue, int);
CaValue ca_try_special(CaRuntime *, CaValue, const char *);
CaValue ca_special(CaRuntime *, CaValue, const char *);
CaValue ca_with_exit(CaRuntime *, CaValue, CaValue);
CaValue ca_stringify(CaRuntime *, CaValue, int);
CaValue ca_join_strings(CaRuntime *, size_t, CaValue *);
CaValue ca_format(CaRuntime *, CaValue, CaValue);
CaValue ca_integer_bitwise(CaRuntime *, CaValue, CaValue, const char *);
CaValue ca_binary_op(CaRuntime *, CaValue, CaValue, const char *);
CaValue ca_extended_call(CaRuntime *, int, int, int, CaValue *, const char **);
void ca_trace_object(CaObj *, void (*)(CaValue, void *), void *);
void ca_foreign_finalize(CaForeign *);
CaValue ca_foreign_attribute(CaRuntime *, CaValue, const char *);
CaValue ca_standard_call(CaRuntime *, int, int, int, CaValue *, const char **);
CaModule *ca_standard_find(CaRuntime *, const char *);
void ca_standard_setup(CaRuntime *);
void ca_standard_arguments(CaRuntime *, int, char **);
void ca_standard_destroy(CaRuntime *);
void ca_error(CaRuntime *, const char *, const char *);
void *ca_allocate(CaRuntime *, size_t, CaTag);
void ca_push(CaRuntime *, CaFrame *, CaValue *, size_t);
void ca_pop(CaRuntime *, CaFrame *);
void ca_collect(CaRuntime *);
void ca_safepoint(CaRuntime *);
void ca_destroy(CaRuntime *);
int ca_enter(CaRuntime *);
void ca_leave(CaRuntime *);
void ca_write_integer(CaRuntime *, CaValue, FILE *);
void ca_write_float(double, FILE *);
int ca_unicode_space(uint32_t);
int ca_unicode_cased(uint32_t);
int ca_unicode_ignorable(uint32_t);
const char *ca_unicode_case(uint32_t, int, size_t *);
int ca_unicode_printable(uint32_t);
CaValue ca_bytes(CaRuntime *, const char *, size_t);
CaValue ca_integer(CaRuntime *, const char *);
CaValue ca_numeric(CaRuntime *, CaValue, CaValue, char);
CaValue ca_negate(CaRuntime *, CaValue);
int ca_integer_compare(CaValue, CaValue);
CaValue ca_integer_true_divide(CaRuntime *, CaValue, CaValue);
CaValue ca_positive_divide(CaRuntime *, CaValue, CaValue);
int ca_numeric_compare(CaRuntime *, CaValue, CaValue, int *);
double ca_as_double(CaRuntime *, CaValue);
void ca_print_integer(CaRuntime *, CaValue);
void ca_print_float(double);
CaValue ca_string(CaRuntime *, const char *, size_t);
CaValue ca_inplace(CaRuntime *, CaValue, CaValue, char);
CaValue ca_range_length(CaRuntime *, CaRange *);
CaValue ca_binary(CaRuntime *, CaValue, CaValue, char);
CaValue ca_unary(CaRuntime *, CaValue, const char *);
CaValue ca_compare(CaRuntime *, CaValue, CaValue, const char *);
int ca_truth(CaRuntime *, CaValue);
CaValue ca_iter(CaRuntime *, CaValue);
int ca_next(CaRuntime *, CaValue, CaValue *);
CaValue ca_call(CaRuntime *, CaValue, int, int, CaValue *, const char **);
CaValue ca_load(CaRuntime *, CaValue, const char *, int);
extern const CaFunction ca_builtin_print, ca_builtin_range;
static inline int ca_long_add(long a, long b, long *out) {
  if ((b > 0 && a > LONG_MAX - b) || (b < 0 && a < LONG_MIN - b))
    return 0;
  *out = a + b;
  return 1;
}
static inline int ca_long_subtract(long a, long b, long *out) {
  if ((b < 0 && a > LONG_MAX + b) || (b > 0 && a < LONG_MIN + b))
    return 0;
  *out = a - b;
  return 1;
}
static inline int ca_long_multiply(long a, long b, long *out) {
  if (a > 0) {
    if ((b > 0 && a > LONG_MAX / b) || (b < 0 && b < LONG_MIN / a))
      return 0;
  } else if (a < 0) {
    if ((b > 0 && a < LONG_MIN / b) || (b < 0 && b < LONG_MAX / a))
      return 0;
  }
  *out = a * b;
  return 1;
}
static inline int ca_range_count(long a, long b, long step, unsigned long *n) {
  if (!step)
    return 0;
  if ((step > 0 && a >= b) || (step < 0 && a <= b)) {
    *n = 0;
    return 1;
  }
  unsigned long distance = step > 0 ? (unsigned long)b - (unsigned long)a
                                    : (unsigned long)a - (unsigned long)b;
  unsigned long stride =
      step > 0 ? (unsigned long)step : 0UL - (unsigned long)step;
  *n = (distance - 1) / stride + 1;
  return *n <= 100000000UL;
}
#endif
