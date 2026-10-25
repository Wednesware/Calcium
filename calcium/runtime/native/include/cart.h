#ifndef CALCIUM_CART_H
#define CALCIUM_CART_H
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* CaRT ABI 1. Tagged primitives, immutable heap objects, tracing roots. */
typedef struct CaObj CaObj;
typedef struct CaRuntime CaRuntime;
typedef struct CaFunction CaFunction;
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
  CA_ITER_T
} CaTag;
typedef struct {
  CaTag tag;
  union {
    int64_t i;
    double f;
    CaObj *obj;
    const CaFunction *fn;
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
static inline CaValue ca_float(double f) {
  return (CaValue){.tag = CA_FLOAT_T, .as.f = f};
}
static inline CaValue ca_function(const CaFunction *f) {
  return (CaValue){.tag = CA_FUNCTION_T, .as.fn = f};
}
typedef struct CaFrame {
  struct CaFrame *previous;
  CaValue *values;
  size_t count;
} CaFrame;
struct CaObj {
  CaObj *next;
  size_t bytes;
  int marked;
  CaTag kind;
};
struct CaRuntime {
  CaObj *heap;
  CaFrame *roots;
  size_t bytes, threshold, collections, allocations, peak_bytes;
  unsigned depth;
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
