#ifndef CALCIUM_CART_H
#define CALCIUM_CART_H
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* CaRT ABI 2. Tagged primitives, module namespaces and traced heap objects,
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
  CA_METHOD_T
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
static inline CaValue ca_int(int64_t i) {
  return (CaValue){.tag = CA_INT_T, .as.i = i};
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
void ca_error(CaRuntime *, const char *, const char *);
void *ca_allocate(CaRuntime *, size_t, CaTag);
void ca_push(CaRuntime *, CaFrame *, CaValue *, size_t);
void ca_pop(CaRuntime *, CaFrame *);
void ca_safepoint(CaRuntime *);
void ca_destroy(CaRuntime *);
int ca_enter(CaRuntime *);
void ca_leave(CaRuntime *);
CaValue ca_integer(CaRuntime *, const char *);
CaValue ca_numeric(CaRuntime *, CaValue, CaValue, char);
CaValue ca_negate(CaRuntime *, CaValue);
int ca_integer_compare(CaValue, CaValue);
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
