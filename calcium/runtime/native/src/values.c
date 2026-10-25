#include "cart.h"
const CaFunction ca_builtin_print = {"print", 0, 0, NULL, NULL, 1};
const CaFunction ca_builtin_range = {"range", 0, 0, NULL, NULL, 2};
static int integer(CaValue v) {
  return v.tag == CA_BOOL_T || v.tag == CA_INT_T || v.tag == CA_BIG_T;
}
static int number(CaValue v) { return integer(v) || v.tag == CA_FLOAT_T; }
CaValue ca_string(CaRuntime *rt, const char *text, size_t length) {
  if (length > SIZE_MAX - sizeof(CaString) - 1) {
    ca_error(rt, "MemoryError", "string too large");
    return CA_UNBOUND;
  }
  CaString *s = ca_allocate(rt, sizeof(CaString) + length + 1, CA_STR_T);
  if (!s)
    return CA_UNBOUND;
  s->length = length;
  memcpy(s->data, text, length);
  return (CaValue){.tag = CA_STR_T, .as.obj = (CaObj *)s};
}
CaValue ca_binary(CaRuntime *rt, CaValue a, CaValue b, char op) {
  if (op == '+' && a.tag == CA_STR_T && b.tag == CA_STR_T) {
    CaString *x = (CaString *)a.as.obj, *y = (CaString *)b.as.obj;
    if (y->length > SIZE_MAX - sizeof(CaString) - 1 - x->length) {
      ca_error(rt, "MemoryError", "string too large");
      return CA_UNBOUND;
    }
    size_t n = x->length + y->length;
    CaString *s = ca_allocate(rt, sizeof(CaString) + n + 1, CA_STR_T);
    if (!s)
      return CA_UNBOUND;
    s->length = n;
    memcpy(s->data, x->data, x->length);
    memcpy(s->data + x->length, y->data, y->length);
    return (CaValue){.tag = CA_STR_T, .as.obj = (CaObj *)s};
  }
  if (op == '*' && (a.tag == CA_STR_T || b.tag == CA_STR_T)) {
    CaValue text = a.tag == CA_STR_T ? a : b, count = a.tag == CA_STR_T ? b : a;
    if (!integer(count)) {
      ca_error(rt, "TypeError", "string repeat count must be an integer");
      return CA_UNBOUND;
    }
    if (count.tag == CA_BIG_T) {
      ca_error(rt, "OverflowError",
               "string repeat count does not fit platform index");
      return CA_UNBOUND;
    }
    CaString *original = (CaString *)text.as.obj;
    uint64_t n = count.as.i > 0 ? (uint64_t)count.as.i : 0;
    if (original->length &&
        n > (SIZE_MAX - sizeof(CaString) - 1) / original->length) {
      ca_error(rt, "OverflowError", "repeated string is too long");
      return CA_UNBOUND;
    }
    size_t length = original->length * (size_t)n;
    CaString *out = ca_allocate(rt, sizeof(CaString) + length + 1, CA_STR_T);
    if (!out)
      return CA_UNBOUND;
    out->length = length;
    for (size_t i = 0; i < length; i += original->length)
      memcpy(out->data + i, original->data, original->length);
    return (CaValue){.tag = CA_STR_T, .as.obj = (CaObj *)out};
  }
  return ca_numeric(rt, a, b, op);
}
int ca_truth(CaRuntime *rt, CaValue v) {
  switch (v.tag) {
  case CA_NONE_T:
    return 0;
  case CA_BOOL_T:
  case CA_INT_T:
    return v.as.i != 0;
  case CA_BIG_T:
    return ((CaBig *)v.as.obj)->count != 0;
  case CA_FLOAT_T:
    return v.as.f != 0;
  case CA_STR_T:
    return ((CaString *)v.as.obj)->length != 0;
  case CA_RANGE_T: {
    CaRange *r = (CaRange *)v.as.obj;
    int s = ca_integer_compare(r->step, ca_int(0));
    return s > 0 ? ca_integer_compare(r->start, r->stop) < 0
                 : ca_integer_compare(r->start, r->stop) > 0;
  }
  case CA_UNBOUND_T:
    ca_error(rt, "UnboundLocalError", "local variable used before assignment");
    return 0;
  default:
    return 1;
  }
}
CaValue ca_unary(CaRuntime *rt, CaValue v, const char *op) {
  if (!strcmp(op, "not"))
    return ca_bool(!ca_truth(rt, v));
  if (!number(v)) {
    ca_error(rt, "TypeError", "bad operand type for unary operation");
    return CA_UNBOUND;
  }
  if (!strcmp(op, "positive"))
    return v.tag == CA_BOOL_T ? ca_int(v.as.i) : v;
  if (v.tag == CA_FLOAT_T)
    return ca_float(-v.as.f);
  return ca_negate(rt, v);
}
static CaValue range_length(CaRuntime *rt, CaRange *range) {
  int positive = ca_integer_compare(range->step, ca_int(0)) > 0;
  CaValue distance = ca_numeric(rt, positive ? range->stop : range->start,
                                positive ? range->start : range->stop, '-');
  if (rt->error)
    return CA_UNBOUND;
  if (ca_integer_compare(distance, ca_int(0)) <= 0)
    return ca_int(0);
  distance = ca_numeric(rt, distance, ca_int(1), '-');
  CaValue stride = positive ? range->step : ca_negate(rt, range->step);
  if (rt->error)
    return CA_UNBOUND;
  CaValue quotient = ca_positive_divide(rt, distance, stride);
  return rt->error ? CA_UNBOUND : ca_numeric(rt, quotient, ca_int(1), '+');
}
CaValue ca_compare(CaRuntime *rt, CaValue a, CaValue b, const char *op) {
  int cmp = 0, unordered = 0, equal = !strcmp(op, "eq"),
      unequal = !strcmp(op, "ne");
  if ((equal || unequal) && a.tag == CA_RANGE_T && b.tag == CA_RANGE_T) {
    CaRange *x = (CaRange *)a.as.obj, *y = (CaRange *)b.as.obj;
    CaValue nx = range_length(rt, x), ny = range_length(rt, y);
    if (rt->error)
      return CA_UNBOUND;
    int same = ca_integer_compare(nx, ny) == 0;
    if (same && ca_integer_compare(nx, ca_int(0)) != 0) {
      same = ca_integer_compare(x->start, y->start) == 0;
      if (same && ca_integer_compare(nx, ca_int(1)) != 0)
        same = ca_integer_compare(x->step, y->step) == 0;
    }
    return ca_bool(unequal ? !same : same);
  }
  if (number(a) && number(b))
    cmp = ca_numeric_compare(rt, a, b, &unordered);
  else if (a.tag == CA_STR_T && b.tag == CA_STR_T) {
    CaString *x = (CaString *)a.as.obj, *y = (CaString *)b.as.obj;
    size_t n = x->length < y->length ? x->length : y->length;
    cmp = memcmp(x->data, y->data, n);
    if (!cmp)
      cmp = (x->length > y->length) - (x->length < y->length);
  } else if (equal || unequal) {
    int same =
        a.tag == b.tag &&
        (a.tag == CA_NONE_T || (a.tag == CA_FUNCTION_T && a.as.fn == b.as.fn));
    return ca_bool(unequal ? !same : same);
  } else {
    ca_error(rt, "TypeError", "values do not support ordering");
    return CA_UNBOUND;
  }
  if (unordered)
    return ca_bool(unequal);
  return ca_bool(equal               ? cmp == 0
                 : unequal           ? cmp != 0
                 : !strcmp(op, "lt") ? cmp < 0
                 : !strcmp(op, "le") ? cmp <= 0
                 : !strcmp(op, "gt") ? cmp > 0
                                     : cmp >= 0);
}
CaValue ca_iter(CaRuntime *rt, CaValue v) {
  if (v.tag != CA_RANGE_T && v.tag != CA_STR_T) {
    ca_error(rt, "TypeError", "object is not iterable");
    return CA_UNBOUND;
  }
  CaIter *it = ca_allocate(rt, sizeof(CaIter), CA_ITER_T);
  if (!it)
    return CA_UNBOUND;
  it->iterable = v;
  it->current = v.tag == CA_RANGE_T ? ((CaRange *)v.as.obj)->start : CA_NONE;
  return (CaValue){.tag = CA_ITER_T, .as.obj = (CaObj *)it};
}
int ca_next(CaRuntime *rt, CaValue v, CaValue *out) {
  CaIter *it = (CaIter *)v.as.obj;
  if (it->iterable.tag == CA_RANGE_T) {
    CaRange *r = (CaRange *)it->iterable.as.obj;
    int positive = ca_integer_compare(r->step, ca_int(0)) > 0;
    int cmp = ca_integer_compare(it->current, r->stop);
    if (positive ? cmp >= 0 : cmp <= 0) {
      *out = CA_UNBOUND;
      return 0;
    }
    *out = it->current;
    it->current = ca_numeric(rt, it->current, r->step, '+');
    return rt->error ? -1 : 1;
  }
  CaString *s = (CaString *)it->iterable.as.obj;
  if (it->index >= s->length) {
    *out = CA_UNBOUND;
    return 0;
  }
  size_t start = it->index++;
  while (it->index < s->length &&
         ((unsigned char)s->data[it->index] & 0xc0) == 0x80)
    it->index++;
  *out = ca_string(rt, s->data + start, it->index - start);
  return rt->error ? -1 : 1;
}
static void print_value(CaRuntime *rt, CaValue v) {
  switch (v.tag) {
  case CA_NONE_T:
    fputs("None", stdout);
    break;
  case CA_BOOL_T:
    fputs(v.as.i ? "True" : "False", stdout);
    break;
  case CA_INT_T:
  case CA_BIG_T:
    ca_print_integer(rt, v);
    break;
  case CA_FLOAT_T:
    ca_print_float(v.as.f);
    break;
  case CA_STR_T: {
    CaString *s = (CaString *)v.as.obj;
    fwrite(s->data, 1, s->length, stdout);
    break;
  }
  case CA_RANGE_T: {
    CaRange *r = (CaRange *)v.as.obj;
    fputs("range(", stdout);
    ca_print_integer(rt, r->start);
    fputs(", ", stdout);
    ca_print_integer(rt, r->stop);
    if (ca_integer_compare(r->step, ca_int(1))) {
      fputs(", ", stdout);
      ca_print_integer(rt, r->step);
    }
    putchar(')');
    break;
  }
  case CA_FUNCTION_T:
    if (v.as.fn->builtin == 2)
      fputs("<class 'range'>", stdout);
    else if (v.as.fn->builtin)
      printf("<built-in function %s>", v.as.fn->name);
    else
      printf("<function %s at %p>", v.as.fn->name, (const void *)v.as.fn);
    break;
  default:
    ca_error(rt, "TypeError", "printing this native value is not supported");
  }
}
static CaValue builtin(CaRuntime *rt, const CaFunction *fn, int n, int nkw,
                       CaValue *v, const char **keys) {
  if (fn->builtin == 1) {
    CaValue sep = CA_NONE, end = CA_NONE;
    int flush = 0;
    for (int i = 0; i < nkw; i++) {
      CaValue value = v[n + i];
      if (!strcmp(keys[i], "flush")) {
        flush = ca_truth(rt, value);
        continue;
      }
      if (!strcmp(keys[i], "file")) {
        if (value.tag != CA_NONE_T) {
          ca_error(rt, "AttributeError", "object has no write attribute");
          return CA_UNBOUND;
        }
        continue;
      }
      if (value.tag != CA_STR_T && value.tag != CA_NONE_T) {
        ca_error(rt, "TypeError",
                 "print separator/end must be a string or None");
        return CA_UNBOUND;
      }
      if (!strcmp(keys[i], "sep"))
        sep = value;
      else if (!strcmp(keys[i], "end"))
        end = value;
      else {
        ca_error(rt, "TypeError", "unsupported print keyword");
        return CA_UNBOUND;
      }
    }
    for (int i = 0; i < n; i++) {
      if (i) {
        if (sep.tag == CA_NONE_T)
          putchar(' ');
        else
          print_value(rt, sep);
      }
      print_value(rt, v[i]);
      if (rt->error)
        return CA_UNBOUND;
    }
    if (end.tag == CA_NONE_T)
      putchar('\n');
    else
      print_value(rt, end);
    if (ferror(stdout))
      ca_error(rt, "OSError", "stdout write failed");
    if (flush && fflush(stdout) != 0)
      ca_error(rt, "OSError", "stdout flush failed");
    return CA_NONE;
  }
  if (nkw || n < 1 || n > 3) {
    ca_error(rt, "TypeError", "range expects 1 to 3 positional arguments");
    return CA_UNBOUND;
  }
  for (int i = 0; i < n; i++)
    if (!integer(v[i])) {
      ca_error(rt, "TypeError", "range arguments must be integers");
      return CA_UNBOUND;
    }
  CaValue start = n == 1 ? ca_int(0) : v[0], stop = n == 1 ? v[0] : v[1],
          step = n == 3 ? v[2] : ca_int(1);
  if (start.tag == CA_BOOL_T)
    start = ca_int(start.as.i);
  if (stop.tag == CA_BOOL_T)
    stop = ca_int(stop.as.i);
  if (step.tag == CA_BOOL_T)
    step = ca_int(step.as.i);
  if (!ca_integer_compare(step, ca_int(0))) {
    ca_error(rt, "ValueError", "range() arg 3 must not be zero");
    return CA_UNBOUND;
  }
  CaRange *r = ca_allocate(rt, sizeof(CaRange), CA_RANGE_T);
  if (!r)
    return CA_UNBOUND;
  r->start = start;
  r->stop = stop;
  r->step = step;
  return (CaValue){.tag = CA_RANGE_T, .as.obj = (CaObj *)r};
}
CaValue ca_call(CaRuntime *rt, CaValue callable, int n, int nkw,
                CaValue *values, const char **keys) {
  if (callable.tag != CA_FUNCTION_T) {
    ca_error(rt, "TypeError", "object is not callable");
    return CA_UNBOUND;
  }
  const CaFunction *fn = callable.as.fn;
  if (fn->builtin)
    return builtin(rt, fn, n, nkw, values, keys);
  if (n > fn->count || n + nkw != fn->count) {
    ca_error(rt, "TypeError", "incorrect number of function arguments");
    return CA_UNBOUND;
  }
  CaValue bound[128] = {0};
  for (int i = 0; i < n; i++)
    bound[i] = values[i];
  for (int i = 0; i < nkw; i++) {
    int slot = -1;
    for (int j = fn->posonly; j < fn->count; j++)
      if (!strcmp(keys[i], fn->parameters[j])) {
        slot = j;
        break;
      }
    if (slot < 0 || bound[slot].tag != CA_UNBOUND_T) {
      ca_error(rt, "TypeError", "unexpected or duplicate keyword argument");
      return CA_UNBOUND;
    }
    bound[slot] = values[n + i];
  }
  for (int i = 0; i < fn->count; i++)
    if (bound[i].tag == CA_UNBOUND_T) {
      ca_error(rt, "TypeError", "missing function argument");
      return CA_UNBOUND;
    }
  return fn->code(rt, bound);
}
CaValue ca_load(CaRuntime *rt, CaValue v, const char *name, int local) {
  if (v.tag != CA_UNBOUND_T)
    return v;
  if (!local) {
    if (!strcmp(name, "print"))
      return ca_function(&ca_builtin_print);
    if (!strcmp(name, "range"))
      return ca_function(&ca_builtin_range);
  }
  char message[220];
  snprintf(message, sizeof message, "name '%s' is not bound", name);
  ca_error(rt, local ? "UnboundLocalError" : "NameError", message);
  return CA_UNBOUND;
}
