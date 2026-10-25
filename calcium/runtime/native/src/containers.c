#include "standard.h"
int ca_is_container(CaValue v) {
  return v.tag >= CA_LIST_T && v.tag <= CA_SET_T;
}
static int eq(CaRuntime *rt, CaValue a, CaValue b) {
  if (a.tag == CA_FLOAT_T && b.tag == CA_FLOAT_T && a.identity &&
      a.identity == b.identity)
    return 1;
  if (a.tag == b.tag &&
      (ca_is_container(a) || a.tag == CA_STR_T || a.tag == CA_METHOD_T ||
       a.tag == CA_RANGE_T || a.tag == CA_ITER_T || a.tag == CA_SLICE_T ||
       a.tag == CA_BIG_T) &&
      a.as.obj == b.as.obj)
    return 1;
  CaValue v = ca_compare(rt, a, b, "eq");
  if (rt->error)
    return 0;
  CaFrame frame;
  ca_push(rt, &frame, &v, 1);
  int result = ca_truth(rt, v);
  ca_pop(rt, &frame);
  return result;
}
static int hashable(CaRuntime *rt, CaValue v) {
  if (v.tag == CA_LIST_T || v.tag == CA_DICT_T || v.tag == CA_SET_T) {
    ca_error(rt, "TypeError", "unhashable container key");
    return 0;
  }
  if (v.tag == CA_TUPLE_T) {
    if (!ca_enter(rt))
      return 0;
    CaContainer *c = (CaContainer *)v.as.obj;
    for (size_t i = 0; i < c->length; i++)
      if (!hashable(rt, c->items[i])) {
        ca_leave(rt);
        return 0;
      }
    ca_leave(rt);
  }
  return 1;
}
static int reserve(CaRuntime *rt, CaContainer *c, size_t n) {
  if (n <= c->capacity)
    return 1;
  if (n > SIZE_MAX / sizeof(CaValue) ||
      n * sizeof(CaValue) > SIZE_MAX - rt->bytes) {
    ca_error(rt, "MemoryError", "container too large");
    return 0;
  }
  size_t size = n < 8 ? 8 : n;
  if (size <= SIZE_MAX / 2 && size * 2 <= SIZE_MAX / sizeof(CaValue))
    size *= 2;
  if (size > SIZE_MAX / sizeof(CaValue) ||
      size * sizeof(CaValue) > SIZE_MAX - rt->bytes) {
    ca_error(rt, "MemoryError", "container too large");
    return 0;
  }
  CaValue *items = realloc(c->items, size * sizeof(CaValue));
  if (!items) {
    ca_error(rt, "MemoryError", "container allocation failed");
    return 0;
  }
  size_t extra = (size - c->capacity) * sizeof(CaValue);
  rt->bytes += extra;
  c->base.bytes += extra;
  if (rt->bytes > rt->peak_bytes)
    rt->peak_bytes = rt->bytes;
  c->items = items;
  c->capacity = size;
  return 1;
}
static size_t locate(CaRuntime *rt, CaContainer *c, CaValue key,
                     size_t stride) {
  if (!hashable(rt, key))
    return c->length;
  for (size_t i = 0; i < c->length && !rt->error; i += stride)
    if (eq(rt, c->items[i], key))
      return i;
  return c->length;
}
void ca_append(CaRuntime *rt, CaValue v, CaValue item) {
  if (v.tag != CA_LIST_T && v.tag != CA_SET_T) {
    ca_error(rt, "TypeError", "append expects list or set");
    return;
  }
  CaContainer *c = (CaContainer *)v.as.obj;
  if (v.tag == CA_SET_T && locate(rt, c, item, 1) < c->length)
    return;
  if (!rt->error && reserve(rt, c, c->length + 1))
    c->items[c->length++] = item;
}
CaValue ca_container(CaRuntime *rt, CaTag tag, size_t n, CaValue *items) {
  CaContainer *c = ca_allocate(rt, sizeof *c, tag);
  if (!c)
    return CA_UNBOUND;
  CaValue result = {.tag = tag, .as.obj = (CaObj *)c};
  if (tag == CA_DICT_T) {
    for (size_t i = 0; i + 1 < n && !rt->error; i += 2)
      ca_setitem(rt, result, items[i], items[i + 1]);
  } else if (tag == CA_SET_T) {
    for (size_t i = 0; i < n && !rt->error; i++)
      ca_append(rt, result, items[i]);
  } else if (reserve(rt, c, n)) {
    if (n)
      memcpy(c->items, items, n * sizeof(CaValue));
    c->length = n;
  }
  return rt->error ? CA_UNBOUND : result;
}
static int index_value(CaRuntime *rt, CaValue key, int64_t *out, int clamp) {
  if (key.tag == CA_INT_T || key.tag == CA_BOOL_T) {
    *out = key.as.i;
    return 1;
  }
  if (key.tag == CA_BIG_T) {
    if (clamp) {
      *out = ((CaBig *)key.as.obj)->sign < 0 ? INT64_MIN : INT64_MAX;
      return 1;
    }
    ca_error(rt, "IndexError", "index does not fit platform index");
    return 0;
  }
  ca_error(rt, "TypeError", "indices must be integers or slices");
  return 0;
}
static size_t text_count(CaString *s) {
  size_t n = 0;
  for (size_t i = 0; i < s->length; i++)
    if (((unsigned char)s->data[i] & 0xc0) != 0x80)
      n++;
  return n;
}
static size_t text_offset(CaString *s, size_t index) {
  size_t i = 0, n = 0;
  while (i < s->length && n < index) {
    i++;
    while (i < s->length && ((unsigned char)s->data[i] & 0xc0) == 0x80)
      i++;
    n++;
  }
  return i;
}
CaValue ca_slice(CaRuntime *rt, CaValue a, CaValue b, CaValue step) {
  CaSlice *s = ca_allocate(rt, sizeof *s, CA_SLICE_T);
  if (!s)
    return CA_UNBOUND;
  s->start = a;
  s->stop = b;
  s->step = step;
  return (CaValue){.tag = CA_SLICE_T, .as.obj = (CaObj *)s};
}
static int64_t clipped(CaRuntime *rt, CaValue value, int64_t n, int64_t step,
                       int stop) {
  if (value.tag == CA_NONE_T)
    return step < 0 ? (stop ? -1 : n - 1) : (stop ? n : 0);
  int64_t out = 0;
  if (!index_value(rt, value, &out, 1))
    return 0;
  if (out < 0) {
    out += n;
    if (out < 0)
      out = step < 0 ? -1 : 0;
  } else if (out >= n)
    out = step < 0 ? n - 1 : n;
  return out;
}
CaValue ca_getitem(CaRuntime *rt, CaValue v, CaValue key) {
  if (v.tag == CA_BYTES_T) {
    CaString *s = (CaString *)v.as.obj;
    int64_t n = (int64_t)s->length;
    if (key.tag == CA_SLICE_T) {
      CaSlice *slice = (CaSlice *)key.as.obj;
      int64_t step = 1;
      if (slice->step.tag != CA_NONE_T &&
          !index_value(rt, slice->step, &step, 1))
        return CA_UNBOUND;
      if (!step) {
        ca_error(rt, "ValueError", "slice step cannot be zero");
        return CA_UNBOUND;
      }
      int64_t start = clipped(rt, slice->start, n, step, 0),
              stop = clipped(rt, slice->stop, n, step, 1);
      if (rt->error)
        return CA_UNBOUND;
      uint64_t distance = step > 0
                              ? (start < stop ? (uint64_t)(stop - start) : 0)
                              : (start > stop ? (uint64_t)(start - stop) : 0),
               stride = step > 0 ? (uint64_t)step : 0ULL - (uint64_t)step;
      size_t count = distance ? (size_t)((distance - 1) / stride + 1) : 0;
      CaValue result = ca_bytes(rt, NULL, count);
      if (rt->error)
        return CA_UNBOUND;
      CaString *out = (CaString *)result.as.obj;
      int64_t index = start;
      for (size_t i = 0; i < count; i++) {
        out->data[i] = s->data[index];
        if (i + 1 < count)
          index += step;
      }
      return result;
    }
    int64_t i;
    if (!index_value(rt, key, &i, 0))
      return CA_UNBOUND;
    if (i < 0)
      i += n;
    if (i < 0 || i >= n) {
      ca_error(rt, "IndexError", "index out of range");
      return CA_UNBOUND;
    }
    return ca_int((unsigned char)s->data[i]);
  }
  if (v.tag == CA_DICT_T) {
    CaContainer *c = (CaContainer *)v.as.obj;
    size_t at = locate(rt, c, key, 2);
    if (rt->error)
      return CA_UNBOUND;
    if (at == c->length) {
      ca_error(rt, "KeyError", "key is not in dictionary");
      return CA_UNBOUND;
    }
    return c->items[at + 1];
  }
  if (v.tag != CA_LIST_T && v.tag != CA_TUPLE_T && v.tag != CA_STR_T) {
    ca_error(rt, "TypeError", "object does not support indexing");
    return CA_UNBOUND;
  }
  size_t length = v.tag == CA_STR_T ? text_count((CaString *)v.as.obj)
                                    : ((CaContainer *)v.as.obj)->length;
  if (length > INT64_MAX) {
    ca_error(rt, "OverflowError", "sequence length exceeds platform index");
    return CA_UNBOUND;
  }
  int64_t n = (int64_t)length;
  if (key.tag == CA_SLICE_T) {
    CaSlice *s = (CaSlice *)key.as.obj;
    int64_t step = 1;
    if (s->step.tag != CA_NONE_T && !index_value(rt, s->step, &step, 1))
      return CA_UNBOUND;
    if (!step) {
      ca_error(rt, "ValueError", "slice step cannot be zero");
      return CA_UNBOUND;
    }
    int64_t start = clipped(rt, s->start, n, step, 0),
            stop = clipped(rt, s->stop, n, step, 1);
    if (rt->error)
      return CA_UNBOUND;
    uint64_t distance = step > 0
                            ? (start < stop ? (uint64_t)(stop - start) : 0)
                            : (start > stop ? (uint64_t)(start - stop) : 0);
    uint64_t stride = step > 0 ? (uint64_t)step : 0ULL - (uint64_t)step;
    size_t count = distance ? (size_t)((distance - 1) / stride + 1) : 0;
    if (v.tag == CA_STR_T) {
      CaString *text = (CaString *)v.as.obj;
      char *out = malloc(text->length + 1);
      if (!out) {
        ca_error(rt, "MemoryError", "slice buffer allocation failed");
        return CA_UNBOUND;
      }
      size_t at = 0;
      int64_t i = start;
      for (size_t j = 0; j < count; j++) {
        size_t a = text_offset(text, (size_t)i),
               b = text_offset(text, (size_t)i + 1);
        memcpy(out + at, text->data + a, b - a);
        at += b - a;
        if (j + 1 < count)
          i += step;
      }
      CaValue result = ca_string(rt, out, at);
      free(out);
      return result;
    }
    CaValue result = ca_container(rt, v.tag, 0, NULL);
    if (rt->error)
      return CA_UNBOUND;
    CaContainer *out = (CaContainer *)result.as.obj,
                *in = (CaContainer *)v.as.obj;
    if (!reserve(rt, out, count))
      return CA_UNBOUND;
    int64_t i = start;
    for (size_t j = 0; j < count; j++) {
      out->items[out->length++] = in->items[i];
      if (j + 1 < count)
        i += step;
    }
    return result;
  }
  int64_t i;
  if (!index_value(rt, key, &i, 0))
    return CA_UNBOUND;
  if (i < 0)
    i += n;
  if (i < 0 || i >= n) {
    ca_error(rt, "IndexError", "sequence index out of range");
    return CA_UNBOUND;
  }
  if (v.tag == CA_STR_T) {
    CaString *s = (CaString *)v.as.obj;
    size_t a = text_offset(s, (size_t)i), b = text_offset(s, (size_t)i + 1);
    return ca_string(rt, s->data + a, b - a);
  }
  return ((CaContainer *)v.as.obj)->items[i];
}
void ca_setitem(CaRuntime *rt, CaValue v, CaValue key, CaValue value) {
  if (v.tag == CA_DICT_T) {
    CaContainer *c = (CaContainer *)v.as.obj;
    size_t at = locate(rt, c, key, 2);
    if (rt->error)
      return;
    if (at < c->length) {
      c->items[at + 1] = value;
      return;
    }
    if (reserve(rt, c, c->length + 2)) {
      c->items[c->length++] = key;
      c->items[c->length++] = value;
    }
    return;
  }
  if (v.tag == CA_LIST_T) {
    CaContainer *c = (CaContainer *)v.as.obj;
    int64_t i;
    if (!index_value(rt, key, &i, 0))
      return;
    if (i < 0)
      i += (int64_t)c->length;
    if (i < 0 || (uint64_t)i >= c->length) {
      ca_error(rt, "IndexError", "list assignment index out of range");
      return;
    }
    c->items[i] = value;
    return;
  }
  ca_error(rt, "TypeError", "object does not support item assignment");
}
CaValue ca_sequence_binary(CaRuntime *rt, CaValue a, CaValue b, char op) {
  CaValue seq = op == '+' ? a
                          : (a.tag == CA_LIST_T || a.tag == CA_TUPLE_T ? a : b),
          other = seq.tag == a.tag && seq.as.obj == a.as.obj ? b : a;
  CaContainer *x = (CaContainer *)seq.as.obj;
  size_t n = x->length;
  if (op == '+') {
    CaContainer *y = (CaContainer *)b.as.obj;
    if (y->length > SIZE_MAX - n) {
      ca_error(rt, "MemoryError", "sequence too large");
      return CA_UNBOUND;
    }
    n += y->length;
  } else {
    int64_t times;
    if (!index_value(rt, other, &times, 0))
      return CA_UNBOUND;
    if (times < 0)
      times = 0;
    if (x->length && (uint64_t)times > SIZE_MAX / x->length) {
      ca_error(rt, "MemoryError", "sequence repeat too large");
      return CA_UNBOUND;
    }
    n = x->length * (size_t)times;
  }
  CaValue result = ca_container(rt, seq.tag, 0, NULL);
  if (rt->error)
    return CA_UNBOUND;
  CaContainer *out = (CaContainer *)result.as.obj;
  if (!reserve(rt, out, n))
    return CA_UNBOUND;
  if (op == '+') {
    CaContainer *y = (CaContainer *)b.as.obj;
    if (x->length)
      memcpy(out->items, x->items, x->length * sizeof(CaValue));
    if (y->length)
      memcpy(out->items + x->length, y->items, y->length * sizeof(CaValue));
  } else if (x->length)
    for (size_t i = 0; i < n; i += x->length)
      memcpy(out->items + i, x->items, x->length * sizeof(CaValue));
  out->length = n;
  return result;
}
CaValue ca_inplace(CaRuntime *rt, CaValue a, CaValue b, char op) {
  if (a.tag != CA_LIST_T || (op != '+' && op != '*'))
    return ca_binary(rt, a, b, op);
  CaContainer *c = (CaContainer *)a.as.obj;
  if (op == '+') {
    CaValue roots[] = {a, b, ca_iter(rt, b), CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 4);
    if (rt->error) {
      ca_pop(rt, &frame);
      return CA_UNBOUND;
    }
    if (a.tag == b.tag && a.as.obj == b.as.obj) {
      size_t n = c->length;
      if (n > SIZE_MAX / 2 || !reserve(rt, c, n * 2)) {
        ca_error(rt, "MemoryError", "list too large");
        ca_pop(rt, &frame);
        return CA_UNBOUND;
      }
      if (n)
        memcpy(c->items + n, c->items, n * sizeof(CaValue));
      c->length = n * 2;
    } else
      while (ca_next(rt, roots[2], &roots[3]) > 0 && !rt->error) {
        ca_append(rt, a, roots[3]);
        if (rt->error)
          break;
        ca_safepoint(rt);
      }
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : a;
  }
  int64_t times;
  if (!index_value(rt, b, &times, 0))
    return CA_UNBOUND;
  if (times <= 0) {
    c->length = 0;
    return a;
  }
  size_t n = c->length;
  if (n && (uint64_t)times > SIZE_MAX / n) {
    ca_error(rt, "MemoryError", "list repeat too large");
    return CA_UNBOUND;
  }
  size_t total = n * (size_t)times;
  if (!reserve(rt, c, total))
    return CA_UNBOUND;
  if (n)
    for (size_t i = n; i < total; i += n)
      memcpy(c->items + i, c->items, n * sizeof(CaValue));
  c->length = total;
  return a;
}
int ca_contains(CaRuntime *rt, CaValue hay, CaValue needle) {
  if (ca_is_container(hay)) {
    CaContainer *c = (CaContainer *)hay.as.obj;
    if (hay.tag == CA_DICT_T || hay.tag == CA_SET_T)
      return locate(rt, c, needle, hay.tag == CA_DICT_T ? 2 : 1) < c->length;
    for (size_t i = 0; i < c->length && !rt->error; i++)
      if (eq(rt, c->items[i], needle))
        return 1;
    return 0;
  }
  if (hay.tag == CA_RANGE_T) {
    CaRange *r = (CaRange *)hay.as.obj;
    if (needle.tag == CA_FLOAT_T) {
      /* Exact finite floats outside int64 are converted without CPython. */
      double f = needle.as.f;
      if (!isfinite(f) || floor(f) != f)
        return 0;
      char buffer[330];
      snprintf(buffer, sizeof buffer, "%.0f", f);
      needle = ca_integer(rt, buffer);
    }
    if (needle.tag != CA_BOOL_T && needle.tag != CA_INT_T &&
        needle.tag != CA_BIG_T)
      return 0;
    int positive = ca_integer_compare(r->step, ca_int(0)) > 0;
    int start = ca_integer_compare(needle, r->start),
        stop = ca_integer_compare(needle, r->stop);
    if (positive ? (start < 0 || stop >= 0) : (start > 0 || stop <= 0))
      return 0;
    CaValue d = ca_numeric(rt, positive ? needle : r->start,
                           positive ? r->start : needle, '-');
    CaValue step = positive ? r->step : ca_negate(rt, r->step);
    CaValue q = ca_positive_divide(rt, d, step),
            p = ca_numeric(rt, q, step, '*');
    return !rt->error && ca_integer_compare(p, d) == 0;
  }
  if (hay.tag == CA_STR_T) {
    if (needle.tag != CA_STR_T) {
      ca_error(rt, "TypeError", "string membership requires string");
      return 0;
    }
    CaString *a = (CaString *)hay.as.obj, *b = (CaString *)needle.as.obj;
    if (b->length > a->length)
      return 0;
    for (size_t i = 0; i <= a->length - b->length; i++)
      if (!memcmp(a->data + i, b->data, b->length))
        return 1;
    return 0;
  }
  ca_error(rt, "TypeError", "membership is not supported for this value");
  return 0;
}
CaValue ca_container_compare(CaRuntime *rt, CaValue a, CaValue b,
                             const char *op) {
  int equal = !strcmp(op, "eq"), unequal = !strcmp(op, "ne");
  if (a.tag != b.tag || !ca_is_container(a)) {
    if (equal || unequal)
      return ca_bool(unequal);
    ca_error(rt, "TypeError", "container types do not support this ordering");
    return CA_UNBOUND;
  }
  if (!ca_enter(rt))
    return CA_UNBOUND;
  CaContainer *x = (CaContainer *)a.as.obj, *y = (CaContainer *)b.as.obj;
  int same = x->length == y->length, less = 0;
  if (a.tag == CA_DICT_T || a.tag == CA_SET_T) {
    if (!equal && !unequal && a.tag == CA_DICT_T) {
      ca_error(rt, "TypeError", "dictionaries do not support ordering");
      ca_leave(rt);
      return CA_UNBOUND;
    }
    size_t stride = a.tag == CA_DICT_T ? 2 : 1;
    int subset = 1;
    for (size_t i = 0; i < x->length && !rt->error; i += stride) {
      size_t at = locate(rt, y, x->items[i], stride);
      if (at == y->length ||
          (stride == 2 && !eq(rt, x->items[i + 1], y->items[at + 1]))) {
        subset = 0;
        break;
      }
    }
    same = same && subset;
    if (!equal && !unequal) {
      int result;
      if (!strcmp(op, "lt") || !strcmp(op, "le"))
        result = subset && (!strcmp(op, "le") || x->length < y->length);
      else {
        ca_leave(rt);
        return ca_container_compare(rt, b, a, !strcmp(op, "gt") ? "lt" : "le");
      }
      ca_leave(rt);
      return ca_bool(result);
    }
  } else {
    size_t n = x->length < y->length ? x->length : y->length;
    less = x->length < y->length;
    for (size_t i = 0; i < n && !rt->error; i++)
      if (!eq(rt, x->items[i], y->items[i])) {
        same = 0;
        if (!equal && !unequal) {
          CaValue v = ca_compare(rt, x->items[i], y->items[i], op);
          ca_leave(rt);
          return v;
        }
        break;
      }
  }
  ca_leave(rt);
  return ca_bool(equal               ? same
                 : unequal           ? !same
                 : !strcmp(op, "lt") ? less
                 : !strcmp(op, "le") ? (less || same)
                 : !strcmp(op, "gt") ? (!less && !same)
                                     : !less);
}
static void repr(CaRuntime *rt, CaValue v) {
  if (v.tag == CA_STR_T)
    ca_repr_string((CaString *)v.as.obj);
  else {
    CaValue repr = ca_stringify(rt, v, 1);
    if (!rt->error) {
      CaString *s = (CaString *)repr.as.obj;
      fwrite(s->data, 1, s->length, stdout);
    }
  }
}
void ca_print_container(CaRuntime *rt, CaValue v) {
  CaContainer *c = (CaContainer *)v.as.obj;
  if (c->printing) {
    fputs(v.tag == CA_LIST_T   ? "[...]"
          : v.tag == CA_DICT_T ? "{...}"
                               : "(...)",
          stdout);
    return;
  }
  if (!ca_enter(rt))
    return;
  c->printing = 1;
  char left = v.tag == CA_LIST_T    ? '['
              : v.tag == CA_TUPLE_T ? '('
                                    : '{',
       right = v.tag == CA_LIST_T    ? ']'
               : v.tag == CA_TUPLE_T ? ')'
                                     : '}';
  if (v.tag == CA_SET_T && !c->length) {
    fputs("set()", stdout);
    c->printing = 0;
    ca_leave(rt);
    return;
  }
  putchar(left);
  for (size_t i = 0; i < c->length && !rt->error; i++) {
    if (i)
      fputs(", ", stdout);
    repr(rt, c->items[i]);
    if (v.tag == CA_DICT_T) {
      fputs(": ", stdout);
      repr(rt, c->items[++i]);
    }
  }
  if (v.tag == CA_TUPLE_T && c->length == 1)
    putchar(',');
  putchar(right);
  c->printing = 0;
  ca_leave(rt);
}
CaValue ca_collection_builtin(CaRuntime *rt, int kind, int n, int nkw,
                              CaValue *v) {
  if (nkw || n > (kind == 3 ? 1 : 1) || (kind == 3 && n != 1)) {
    ca_error(rt, "TypeError", "invalid collection builtin arguments");
    return CA_UNBOUND;
  }
  if (kind == 3) {
    if (ca_is_container(v[0])) {
      size_t n = ((CaContainer *)v[0].as.obj)->length;
      return ca_int((int64_t)(v[0].tag == CA_DICT_T ? n / 2 : n));
    }
    if (v[0].tag == CA_STR_T || v[0].tag == CA_BYTES_T)
      return ca_int((int64_t)(v[0].tag == CA_BYTES_T
                                  ? ((CaString *)v[0].as.obj)->length
                                  : text_count((CaString *)v[0].as.obj)));
    if (v[0].tag == CA_RANGE_T) {
      CaValue n = ca_range_length(rt, (CaRange *)v[0].as.obj);
      if (n.tag == CA_BIG_T) {
        ca_error(rt, "OverflowError",
                 "range length does not fit platform index");
        return CA_UNBOUND;
      }
      return n;
    }
    if (v[0].tag == CA_INSTANCE_T) {
      CaValue method = ca_try_special(rt, v[0], "__len__");
      if (method.tag == CA_UNBOUND_T) {
        ca_error(rt, "TypeError", "object has no len()");
        return CA_UNBOUND;
      }
      CaValue length = ca_call(rt, method, 0, 0, NULL, NULL);
      if (rt->error)
        return CA_UNBOUND;
      if (length.tag == CA_BIG_T) {
        ca_error(rt, "OverflowError", "length does not fit platform index");
        return CA_UNBOUND;
      }
      if (length.tag != CA_INT_T && length.tag != CA_BOOL_T) {
        ca_error(rt, "TypeError", "__len__ must return an integer");
        return CA_UNBOUND;
      }
      if (length.as.i < 0) {
        ca_error(rt, "ValueError", "__len__ must be non-negative");
        return CA_UNBOUND;
      }
      return ca_int(length.as.i);
    }
    if (v[0].tag == CA_FOREIGN_T &&
        ((CaForeign *)v[0].as.obj)->kind == CA_F_DICT_VIEW)
      return ca_int(
          ((CaContainer *)((CaForeign *)v[0].as.obj)->state.as.obj)->length /
          2);
    ca_error(rt, "TypeError", "object has no supported length");
    return CA_UNBOUND;
  }
  CaTag tag = kind == 4   ? CA_LIST_T
              : kind == 5 ? CA_TUPLE_T
              : kind == 6 ? CA_DICT_T
                          : CA_SET_T;
  if (kind == 5 && n && v[0].tag == CA_TUPLE_T)
    return v[0];
  CaValue result = ca_container(rt, tag, 0, NULL);
  if (rt->error || !n)
    return result;
  if (tag == CA_DICT_T) {
    if (v[0].tag == CA_DICT_T) {
      CaContainer *c = (CaContainer *)v[0].as.obj;
      return ca_container(rt, tag, c->length, c->items);
    }
    ca_error(rt, "TypeError", "native dict() currently requires a dictionary");
    return CA_UNBOUND;
  }
  /* Iteration is a safepoint: root construction and iterator while nested
     container allocations are collected, including empty-result cases. */
  CaValue roots[] = {result, ca_iter(rt, v[0]), CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 3);
  if (!rt->error) {
    while (ca_next(rt, roots[1], &roots[2]) > 0 && !rt->error) {
      CaContainer *c = (CaContainer *)result.as.obj;
      if (tag == CA_TUPLE_T) {
        if (reserve(rt, c, c->length + 1))
          c->items[c->length++] = roots[2];
      } else
        ca_append(rt, result, roots[2]);
      if (rt->error)
        break;
      ca_safepoint(rt);
    }
  }
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : result;
}
void ca_extend(CaRuntime *rt, CaValue target, CaValue source) {
  if (target.tag == CA_LIST_T) {
    ca_inplace(rt, target, source, '+');
    return;
  }
  if (target.tag != CA_SET_T) {
    ca_error(rt, "TypeError", "extend target must be list or set");
    return;
  }
  CaValue roots[] = {target, source, ca_iter(rt, source), CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 4);
  if (!rt->error)
    while (ca_next(rt, roots[2], &roots[3]) > 0 && !rt->error) {
      ca_append(rt, target, roots[3]);
      if (rt->error)
        break;
      ca_safepoint(rt);
    }
  ca_pop(rt, &frame);
}
void ca_kwargs_merge(CaRuntime *rt, CaValue target, CaValue source,
                     int unique) {
  if (target.tag != CA_DICT_T || source.tag != CA_DICT_T) {
    ca_error(rt, "TypeError", "keyword expansion requires dictionary");
    return;
  }
  CaContainer *in = (CaContainer *)source.as.obj,
              *out = (CaContainer *)target.as.obj;
  for (size_t i = 0; i < in->length && !rt->error; i += 2) {
    if (unique && locate(rt, out, in->items[i], 2) < out->length) {
      ca_error(rt, "TypeError", "multiple values for keyword");
      return;
    }
    ca_setitem(rt, target, in->items[i], in->items[i + 1]);
  }
}
void ca_kwargs_set(CaRuntime *rt, CaValue target, const char *name,
                   CaValue value) {
  CaValue key = ca_string(rt, name, strlen(name));
  if (rt->error)
    return;
  CaContainer *out = (CaContainer *)target.as.obj;
  if (locate(rt, out, key, 2) < out->length) {
    ca_error(rt, "TypeError", "multiple values for keyword");
    return;
  }
  ca_setitem(rt, target, key, value);
}
CaValue ca_call_ex(CaRuntime *rt, CaValue function, CaValue args,
                   CaValue kwargs) {
  CaContainer *a = (CaContainer *)args.as.obj,
              *k = (CaContainer *)kwargs.as.obj;
  if (a->length + k->length / 2 > 128) {
    ca_error(rt, "OverflowError", "native expanded call exceeds 128 arguments");
    return CA_UNBOUND;
  }
  CaValue values[128];
  const char *keys[128];
  for (size_t i = 0; i < a->length; i++)
    values[i] = a->items[i];
  for (size_t i = 0; i < k->length; i += 2) {
    if (k->items[i].tag != CA_STR_T) {
      ca_error(rt, "TypeError", "keywords must be strings");
      return CA_UNBOUND;
    }
    CaString *s = (CaString *)k->items[i].as.obj;
    if (memchr(s->data, 0, s->length)) {
      ca_error(rt, "TypeError", "embedded NUL keyword is unsupported");
      return CA_UNBOUND;
    }
    keys[i / 2] = s->data;
    values[a->length + i / 2] = k->items[i + 1];
  }
  return ca_call(rt, function, (int)a->length, (int)(k->length / 2), values,
                 keys);
}
CaValue ca_unpack(CaRuntime *rt, CaValue value, size_t n) {
  CaValue roots[] = {ca_container(rt, CA_LIST_T, 0, NULL), value, CA_UNBOUND,
                     CA_UNBOUND};
  if (rt->error)
    return CA_UNBOUND;
  roots[2] = ca_iter(rt, value);
  CaFrame frame;
  ca_push(rt, &frame, roots, 4);
  size_t count = 0;
  while (!rt->error && count <= n && ca_next(rt, roots[2], &roots[3]) > 0) {
    ca_append(rt, roots[0], roots[3]);
    count++;
    if (rt->error)
      break;
    ca_safepoint(rt);
  }
  if (!rt->error && count != n)
    ca_error(rt, "ValueError", "incorrect number of values to unpack");
  CaValue result = roots[0];
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : result;
}
CaValue ca_dict_pop(CaRuntime *rt, int n, int nkw, CaValue *args,
                    const char **keys) {
  (void)keys;
  if (nkw || n < 2 || n > 3 || args[0].tag != CA_DICT_T) {
    ca_error(rt, "TypeError", "dict.pop expects key and optional default");
    return CA_UNBOUND;
  }
  CaContainer *c = (CaContainer *)args[0].as.obj;
  size_t at = locate(rt, c, args[1], 2);
  if (rt->error)
    return CA_UNBOUND;
  if (at == c->length) {
    if (n == 3)
      return args[2];
    ca_error(rt, "KeyError", "dictionary key not found");
    return CA_UNBOUND;
  }
  CaValue result = c->items[at + 1];
  memmove(c->items + at, c->items + at + 2,
          (c->length - at - 2) * sizeof(CaValue));
  c->length -= 2;
  c->items[c->length] = c->items[c->length + 1] = CA_UNBOUND;
  return result;
}
CaValue ca_bound_method(CaRuntime *rt, CaValue v, const char *name) {
  if (v.tag == CA_DICT_T && !strcmp(name, "pop"))
    return ca_native_method(rt, v, N_DICT_POP);
  if (v.tag == CA_STR_T || v.tag == CA_BYTES_T) {
    const char *names[] = {"encode", "decode",     "join",     "replace",
                           "lower",  "upper",      "strip",    "lstrip",
                           "rstrip", "startswith", "endswith", "split",
                           "rsplit", "find",       "count"};
    int ids[] = {N_TEXT_ENCODE,     N_TEXT_DECODE,   N_TEXT_JOIN,
                 N_TEXT_REPLACE,    N_TEXT_LOWER,    N_TEXT_UPPER,
                 N_TEXT_STRIP,      N_TEXT_LSTRIP,   N_TEXT_RSTRIP,
                 N_TEXT_STARTSWITH, N_TEXT_ENDSWITH, N_TEXT_SPLIT,
                 N_TEXT_RSplit,     N_TEXT_FIND,     N_TEXT_COUNT};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++)
      if (!strcmp(name, names[i]) &&
          !(v.tag == CA_BYTES_T && ids[i] == N_TEXT_ENCODE) &&
          !(v.tag == CA_STR_T && ids[i] == N_TEXT_DECODE))
        return ca_native_method(rt, v, ids[i]);
  }
  if (v.tag == CA_DICT_T && (!strcmp(name, "items") || !strcmp(name, "keys") ||
                             !strcmp(name, "values")))
    return ca_native_method(rt, v,
                            !strcmp(name, "items")  ? N_DICT_ITEMS
                            : !strcmp(name, "keys") ? N_DICT_KEYS
                                                    : N_DICT_VALUES);
  int method = 0;
  if (v.tag == CA_LIST_T) {
    if (!strcmp(name, "append"))
      method = 1;
    else if (!strcmp(name, "extend"))
      method = 2;
    else if (!strcmp(name, "pop"))
      method = 3;
  }
  if (v.tag == CA_DICT_T) {
    if (!strcmp(name, "get"))
      method = 4;
    else if (!strcmp(name, "update"))
      method = 5;
  }
  if (v.tag == CA_SET_T && !strcmp(name, "add"))
    method = 6;
  if (v.tag == CA_LIST_T || v.tag == CA_DICT_T || v.tag == CA_SET_T) {
    if (!strcmp(name, "clear"))
      method = 7;
    if (!strcmp(name, "copy"))
      method = 8;
  }
  if (!method) {
    ca_error(rt, "AttributeError",
             "object has no supported native attribute or method");
    return CA_UNBOUND;
  }
  CaMethod *m = ca_allocate(rt, sizeof *m, CA_METHOD_T);
  if (!m)
    return CA_UNBOUND;
  m->receiver = v;
  m->method = method;
  return (CaValue){.tag = CA_METHOD_T, .as.obj = (CaObj *)m};
}
CaValue ca_method_call(CaRuntime *rt, CaValue v, int n, int nkw,
                       CaValue *args) {
  CaMethod *m = (CaMethod *)v.as.obj;
  CaValue target = m->receiver;
  CaContainer *c = (CaContainer *)target.as.obj;
  int min = m->method <= 2 || m->method == 4 || m->method == 6 ? 1 : 0;
  int max = m->method == 4 ? 2 : m->method >= 7 ? 0 : 1;
  if (nkw || n < min || n > max) {
    ca_error(rt, "TypeError", "invalid native method arguments");
    return CA_UNBOUND;
  }
  switch (m->method) {
  case 1:
  case 6:
    ca_append(rt, target, args[0]);
    return CA_NONE;
  case 2:
    ca_extend(rt, target, args[0]);
    return CA_NONE;
  case 3: {
    int64_t i = -1;
    if (n && !index_value(rt, args[0], &i, 0))
      return CA_UNBOUND;
    if (i < 0)
      i += (int64_t)c->length;
    if (i < 0 || (uint64_t)i >= c->length) {
      ca_error(rt, "IndexError", "pop index out of range");
      return CA_UNBOUND;
    }
    CaValue result = c->items[i];
    memmove(c->items + i, c->items + i + 1,
            (c->length - (size_t)i - 1) * sizeof(CaValue));
    c->length--;
    return result;
  }
  case 4: {
    size_t i = locate(rt, c, args[0], 2);
    return i < c->length ? c->items[i + 1] : n == 2 ? args[1] : CA_NONE;
  }
  case 5:
    if (n)
      ca_kwargs_merge(rt, target, args[0], 0);
    return CA_NONE;
  case 7:
    c->length = 0;
    return CA_NONE;
  case 8:
    return ca_container(rt, target.tag, c->length, c->items);
  }
  return CA_UNBOUND;
}
CaValue ca_dict_constructor(CaRuntime *rt, int n, int nkw, CaValue *args,
                            const char **keys) {
  if (n > 1) {
    ca_error(rt, "TypeError", "dict expects at most one positional argument");
    return CA_UNBOUND;
  }
  CaValue roots[] = {ca_container(rt, CA_DICT_T, 0, NULL), CA_UNBOUND,
                     CA_UNBOUND, CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 4);
  if (n && !rt->error) {
    if (args[0].tag == CA_DICT_T)
      ca_kwargs_merge(rt, roots[0], args[0], 0);
    else {
      roots[1] = ca_iter(rt, args[0]);
      if (!rt->error)
        while (ca_next(rt, roots[1], &roots[2]) > 0 && !rt->error) {
          roots[3] = ca_unpack(rt, roots[2], 2);
          if (rt->error)
            break;
          CaContainer *pair = (CaContainer *)roots[3].as.obj;
          ca_setitem(rt, roots[0], pair->items[0], pair->items[1]);
          if (rt->error)
            break;
          ca_safepoint(rt);
        }
    }
  }
  for (int i = 0; i < nkw && !rt->error; i++) {
    CaValue key = ca_string(rt, keys[i], strlen(keys[i]));
    if (rt->error)
      break;
    ca_setitem(rt, roots[0], key, args[n + i]);
  }
  CaValue result = roots[0];
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : result;
}
