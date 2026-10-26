#include "standard.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <strings.h>
static const CaFunction extra[] = {{"isinstance", 0, 0, NULL, NULL, 10},
                                   {"issubclass", 0, 0, NULL, NULL, 11},
                                   {"getattr", 0, 0, NULL, NULL, 12},
                                   {"setattr", 0, 0, NULL, NULL, 13},
                                   {"hasattr", 0, 0, NULL, NULL, 14},
                                   {"staticmethod", 0, 0, NULL, NULL, 20},
                                   {"classmethod", 0, 0, NULL, NULL, 21},
                                   {"property", 0, 0, NULL, NULL, 22},
                                   {"repr", 0, 0, NULL, NULL, 23},
                                   {"ascii", 0, 0, NULL, NULL, 24},
                                   {"sorted", 0, 0, NULL, NULL, 25},
                                   {"sum", 0, 0, NULL, NULL, 26},
                                   {"min", 0, 0, NULL, NULL, 27},
                                   {"max", 0, 0, NULL, NULL, 28},
                                   {"abs", 0, 0, NULL, NULL, 29},
                                   {"round", 0, 0, NULL, NULL, 30},
                                   {"enumerate", 0, 0, NULL, NULL, 31},
                                   {"zip", 0, 0, NULL, NULL, 32},
                                   {"iter", 0, 0, NULL, NULL, 33},
                                   {"next", 0, 0, NULL, NULL, 34},
                                   {"callable", 0, 0, NULL, NULL, 35},
                                   {"type", 0, 0, NULL, NULL, 36}};
CaValue ca_builtin_lookup(const char *name) {
#if CART_PY_MINOR < 13
  if (!strcmp(name, "PythonFinalizationError"))
    return CA_UNBOUND;
#endif
  CaValue more = ca_builtin_more_lookup(name);
  if (more.tag != CA_UNBOUND_T)
    return more;
  if (!strcmp(name, "__debug__"))
    return ca_bool(1);
  if (!strcmp(name, "slice"))
    return ca_type(CA_T_SLICE);
  if (!strcmp(name, "type"))
    return ca_type(CA_T_TYPE);
  CaValue error = ca_exception_type(name);
  if (error.tag != CA_UNBOUND_T &&
      (error.as.i <= CA_T_KEYBOARDINTERRUPT || error.as.i >= CA_T_UNICODEERROR))
    return error;
  static const char *types[] = {"object", "str",  "int",
                                "float",  "bool", "bytes"};
  for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
    if (!strcmp(name, types[i]))
      return ca_type(CA_T_OBJECT + (long)i);
  for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++)
    if (!strcmp(name, extra[i].name))
      return ca_function(extra + i);
  return CA_UNBOUND;
}
CaValue ca_bytes(CaRuntime *rt, const char *text, size_t length) {
  if (length > SIZE_MAX - sizeof(CaString) - 1) {
    ca_error(rt, "MemoryError", "bytes object too large");
    return CA_UNBOUND;
  }
  CaString *s = ca_allocate(rt, sizeof *s + length + 1, CA_BYTES_T);
  if (!s)
    return CA_UNBOUND;
  s->length = length;
  if (text && length)
    memcpy(s->data, text, length);
  return (CaValue){.tag = CA_BYTES_T, .as.obj = (CaObj *)s};
}
static CaValue integer_parse(CaRuntime *rt, CaValue input, int base) {
  if (input.tag == CA_INT_T || input.tag == CA_BIG_T)
    return input;
  if (input.tag == CA_BOOL_T)
    return ca_int(input.as.i);
  if (input.tag == CA_FLOAT_T) {
    if (!isfinite(input.as.f)) {
      ca_error(rt, isnan(input.as.f) ? "ValueError" : "OverflowError",
               "cannot convert non-finite float to integer");
      return CA_UNBOUND;
    }
    char text[330];
    snprintf(text, sizeof text, "%.0f", trunc(input.as.f));
    return ca_integer(rt, text);
  }
  if (input.tag != CA_STR_T && input.tag != CA_BYTES_T) {
    ca_error(rt, "TypeError", "int expects a string or number");
    return CA_UNBOUND;
  }
  CaString *s = (CaString *)input.as.obj;
  size_t at = 0, end = s->length;
  while (at < end && isspace((unsigned char)s->data[at]))
    at++;
  while (end > at && isspace((unsigned char)s->data[end - 1]))
    end--;
  int negative = 0;
  if (at < end && (s->data[at] == '+' || s->data[at] == '-'))
    negative = s->data[at++] == '-';
  if (base && (base < 2 || base > 36)) {
    ca_error(rt, "ValueError", "int base must be 0 or between 2 and 36");
    return CA_UNBOUND;
  }
  int zero_base = !base, prefix = 0;
  if (at + 1 < end && s->data[at] == '0') {
    int c = tolower((unsigned char)s->data[at + 1]);
    int detected = c == 'x' ? 16 : c == 'o' ? 8 : c == 'b' ? 2 : 0;
    if (detected && (!base || base == detected)) {
      base = detected;
      at += 2;
      prefix = 1;
    }
  }
  if (!base)
    base = 10;
  if (prefix && at < end && s->data[at] == '_')
    at++;
  CaValue value = ca_int(0);
  int digits = 0, underscore = 0, leading_zero = at < end && s->data[at] == '0';
  for (; at < end; at++) {
    int c = (unsigned char)s->data[at];
    if (c == '_') {
      if (!digits || underscore)
        goto invalid;
      underscore = 1;
      continue;
    }
    int d = c >= '0' && c <= '9'   ? c - '0'
            : c >= 'a' && c <= 'z' ? c - 'a' + 10
            : c >= 'A' && c <= 'Z' ? c - 'A' + 10
                                   : -1;
    if (d < 0 || d >= base)
      goto invalid;
    if (zero_base && !prefix && leading_zero && d)
      goto invalid;
    value = ca_numeric(rt, value, ca_int(base), '*');
    if (rt->error)
      return CA_UNBOUND;
    value = ca_numeric(rt, value, ca_int(d), '+');
    if (rt->error)
      return CA_UNBOUND;
    digits++;
    underscore = 0;
  }
  if (!digits || underscore)
    goto invalid;
  return negative ? ca_negate(rt, value) : value;
invalid:
  ca_error(rt, "ValueError", "invalid literal for int()");
  return CA_UNBOUND;
}
static int string_name(CaRuntime *rt, CaValue value, const char **name) {
  if (value.tag != CA_STR_T) {
    ca_error(rt, "TypeError", "attribute name must be string");
    return 0;
  }
  CaString *s = (CaString *)value.as.obj;
  if (memchr(s->data, 0, s->length)) {
    ca_error(rt, "AttributeError", "embedded null in attribute name");
    return 0;
  }
  *name = s->data;
  return 1;
}
CaValue ca_extended_call(CaRuntime *rt, int kind, int n, int nkw, CaValue *args,
                         const char **keys) {
  if (kind >= CA_T_MAP && kind <= CA_T_REVERSED)
    return ca_builtin_more(rt,
                           kind == CA_T_MAP      ? B_MAP
                           : kind == CA_T_FILTER ? B_FILTER
                                                 : B_REVERSED,
                           n, nkw, args, keys);
  if (kind >= 400)
    return ca_builtin_more(rt, kind, n, nkw, args, keys);
  if (kind == 30)
    return ca_builtin_more(rt, B_ROUND, n, nkw, args, keys);
  if (kind == CA_T_SLICE)
    return ca_builtin_more(rt, B_SLICE, n, nkw, args, keys);
  if (kind == 33 && n == 2 && !nkw)
    return ca_builtin_more(rt, B_CALL_ITER, n, nkw, args, keys);
  if (kind == CA_T_RANGE)
    return ca_call(rt, ca_function(&ca_builtin_range), n, nkw, args, keys);
  if (kind == CA_T_TYPE || kind == 36) {
    if (n != 1 || nkw)
      goto arguments;
    return ca_type_of(args[0]);
  }
  if (kind >= CA_T_LIST && kind <= CA_T_SET)
    return ca_collection_builtin(rt, kind - CA_T_LIST + 4, n, nkw, args);
  if (kind == CA_T_OBJECT) {
    if (n || nkw) {
      ca_error(rt, "TypeError", "object() takes no arguments");
      return CA_UNBOUND;
    }
    CaValue type = ca_class(rt, "object", 0, NULL);
    return ca_object_call(rt, type, 0, 0, NULL, NULL);
  }
  if (kind == CA_T_BOOL) {
    if (nkw || n > 1)
      goto arguments;
    return ca_bool(n ? ca_truth(rt, args[0]) : 0);
  }
  if (kind == CA_T_STR) {
    if (nkw || n > 1)
      goto arguments;
    return n ? ca_stringify(rt, args[0], 0) : ca_string(rt, "", 0);
  }
  if (kind == CA_T_INT) {
    int base = 10;
    if (n > 2 || nkw > 1 || (nkw && (strcmp(keys[0], "base") || n > 1)))
      goto arguments;
    CaValue b = n == 2 ? args[1] : nkw ? args[n] : ca_int(10);
    if (b.tag != CA_INT_T && b.tag != CA_BOOL_T)
      goto arguments;
    base = (int)b.as.i;
    if ((n == 2 || nkw) &&
        (!n || (args[0].tag != CA_STR_T && args[0].tag != CA_BYTES_T))) {
      ca_error(rt, "TypeError", "explicit base requires a string");
      return CA_UNBOUND;
    }
    return n ? integer_parse(rt, args[0], base) : ca_int(0);
  }
  if (kind == CA_T_FLOAT) {
    if (nkw || n > 1)
      goto arguments;
    if (!n)
      return ca_float(0);
    if (args[0].tag == CA_STR_T || args[0].tag == CA_BYTES_T) {
      CaString *s = (CaString *)args[0].as.obj;
      char *copy = malloc(s->length + 1);
      if (!copy) {
        ca_error(rt, "MemoryError", "float conversion allocation failed");
        return CA_UNBOUND;
      }
      size_t at = 0;
      int invalid = 0;
      for (size_t i = 0; i < s->length; i++) {
        if (s->data[i] == '_') {
          if (!i || i + 1 == s->length ||
              !isdigit((unsigned char)s->data[i - 1]) ||
              !isdigit((unsigned char)s->data[i + 1]))
            invalid = 1;
        } else
          copy[at++] = s->data[i];
      }
      copy[at] = 0;
      char *end = NULL;
      double f = strtod(copy, &end);
      char *start = copy;
      while (isspace((unsigned char)*start))
        start++;
      if (!strncasecmp(start, "0x", 2) || !strncasecmp(start, "+0x", 3) ||
          !strncasecmp(start, "-0x", 3))
        invalid = 1;
      int consumed = end != copy;
      while (end && isspace((unsigned char)*end))
        end++;
      invalid |=
          !consumed || !end || *end || memchr(s->data, 0, s->length) != NULL;
      free(copy);
      if (invalid) {
        ca_error(rt, "ValueError", "could not convert string to float");
        return CA_UNBOUND;
      }
      return ca_float(f);
    }
    double f = ca_as_double(rt, args[0]);
    return rt->error ? CA_UNBOUND : ca_float(f);
  }
  if (kind == CA_T_BYTES) {
    if (nkw || n > 1)
      goto arguments;
    if (!n)
      return ca_bytes(rt, "", 0);
    if (args[0].tag == CA_BYTES_T)
      return args[0];
    if (args[0].tag == CA_INT_T || args[0].tag == CA_BOOL_T) {
      if (args[0].as.i < 0) {
        ca_error(rt, "ValueError", "negative count");
        return CA_UNBOUND;
      }
      return ca_bytes(rt, NULL, (size_t)args[0].as.i);
    }
    CaValue roots[] = {ca_collection_builtin(rt, 4, 1, 0, args), CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 2);
    if (rt->error) {
      ca_pop(rt, &frame);
      return CA_UNBOUND;
    }
    CaContainer *c = (CaContainer *)roots[0].as.obj;
    roots[1] = ca_bytes(rt, NULL, c->length);
    if (!rt->error) {
      CaString *out = (CaString *)roots[1].as.obj;
      for (size_t i = 0; i < c->length; i++) {
        if ((c->items[i].tag != CA_INT_T && c->items[i].tag != CA_BOOL_T) ||
            c->items[i].as.i < 0 || c->items[i].as.i > 255) {
          ca_error(rt, "ValueError", "bytes must be in range(0,256)");
          break;
        }
        out->data[i] = (char)c->items[i].as.i;
      }
    }
    CaValue result = roots[1];
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : result;
  }
  if (kind == 10 || kind == 11) {
    if (n != 2 || nkw)
      goto arguments;
    return ca_bool(kind == 10 ? ca_isinstance(rt, args[0], args[1])
                              : ca_subclass(rt, args[0], args[1]));
  }
  if (kind == 12 || kind == 13 || kind == 14) {
    if (nkw || n < (kind == 13 ? 3 : 2) || n > (kind == 14 ? 2 : 3))
      goto arguments;
    const char *name;
    if (!string_name(rt, args[1], &name))
      return CA_UNBOUND;
    if (kind == 13) {
      ca_set_attribute(rt, args[0], name, args[2]);
      return rt->error ? CA_UNBOUND : CA_NONE;
    }
    CaValue value = ca_attribute(rt, args[0], name, 0);
    if (rt->error && !strcmp(rt->error, "AttributeError")) {
      if (kind == 14 || n == 3) {
        ca_exception_take(rt);
        return kind == 14 ? ca_bool(0) : args[2];
      }
    }
    return kind == 14 && !rt->error ? ca_bool(1) : value;
  }
  if (kind >= 20 && kind <= 22) {
    if (n != 1 || nkw)
      goto arguments;
    CaForeign *f = ca_allocate(rt, sizeof *f, CA_FOREIGN_T);
    if (!f)
      return CA_UNBOUND;
    f->kind = kind - 19;
    f->state = args[0];
    return (CaValue){.tag = CA_FOREIGN_T, .as.obj = (CaObj *)f};
  }
  if (kind == 23 || kind == 24) {
    if (n != 1 || nkw)
      goto arguments;
    return ca_stringify(rt, args[0], kind == 24 ? 2 : 1);
  }
  if (kind == 25) {
    if (n != 1)
      goto arguments;
    CaValue key = CA_NONE;
    int reverse = 0;
    for (int i = 0; i < nkw; i++) {
      if (!strcmp(keys[i], "key"))
        key = args[n + i];
      else if (!strcmp(keys[i], "reverse")) {
        if (args[n + i].tag != CA_INT_T && args[n + i].tag != CA_BOOL_T)
          goto arguments;
        reverse = args[n + i].as.i != 0;
      } else
        goto arguments;
    }
    CaValue roots[] = {ca_collection_builtin(rt, 4, 1, 0, args), CA_UNBOUND,
                       key};
    CaFrame frame;
    ca_push(rt, &frame, roots, 3);
    if (rt->error) {
      ca_pop(rt, &frame);
      return CA_UNBOUND;
    }
    CaContainer *c = (CaContainer *)roots[0].as.obj;
    roots[1] = ca_container(rt, CA_LIST_T, 0, NULL);
    for (size_t i = 0; i < c->length && !rt->error; i++) {
      CaValue v = key.tag == CA_NONE_T
                      ? c->items[i]
                      : ca_call(rt, key, 1, 0, &c->items[i], NULL);
      if (!rt->error)
        ca_append(rt, roots[1], v);
    }
    if (!rt->error) {
      CaContainer *k = (CaContainer *)roots[1].as.obj;
      /* Root both merge buffers: rich comparisons may run arbitrary code and
       * GC. */
      size_t length = c->length;
      if (length > SIZE_MAX / (2 * sizeof(CaValue))) {
        ca_error(rt, "MemoryError", "sort is too large");
      } else if (length > 1) {
        CaValue *buffer = calloc(2 * length, sizeof(CaValue));
        if (!buffer) {
          ca_error(rt, "MemoryError", "sort allocation failed");
        } else {
          CaFrame scratch;
          ca_push(rt, &scratch, buffer, 2 * length);
          for (size_t width = 1; width < length && !rt->error;) {
            memcpy(buffer, c->items, length * sizeof(CaValue));
            memcpy(buffer + length, k->items, length * sizeof(CaValue));
            for (size_t begin = 0; begin < length && !rt->error;) {
              size_t middle =
                  begin + (length - begin < width ? length - begin : width);
              size_t end =
                  middle + (length - middle < width ? length - middle : width);
              size_t left = begin, right = middle;
              for (size_t out = begin; out < end && !rt->error; out++) {
                int use_right = left == middle;
                if (left < middle && right < end) {
                  CaValue cmp = ca_compare(
                      rt, buffer[length + (reverse ? left : right)],
                      buffer[length + (reverse ? right : left)], "lt");
                  CaFrame comparison;
                  ca_push(rt, &comparison, &cmp, 1);
                  use_right = !rt->error && ca_truth(rt, cmp);
                  ca_pop(rt, &comparison);
                }
                size_t index = use_right && right < end ? right++ : left++;
                c->items[out] = buffer[index];
                k->items[out] = buffer[length + index];
              }
              begin = end;
            }
            if (width > length / 2)
              break;
            width *= 2;
          }
          ca_pop(rt, &scratch);
          free(buffer);
        }
      }
    }
    CaValue result = roots[0];
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : result;
  }
  if (kind == 31 || kind == 32) {
    if (kind == 32 && nkw) {
      ca_error(rt, "NotImplementedError",
               "zip keyword options are not implemented");
      return CA_UNBOUND;
    }
    if (kind == 31 && (n < 1 || n > 2 || nkw > 1 ||
                       (nkw && (strcmp(keys[0], "start") || n == 2))))
      goto arguments;
    CaValue roots[] = {CA_UNBOUND, CA_UNBOUND, CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 3);
    if (kind == 31) {
      roots[0] = ca_iter(rt, args[0]);
      roots[1] = n == 2 ? args[1] : nkw ? args[n] : ca_int(0);
      if (roots[1].tag == CA_BOOL_T)
        roots[1] = ca_int(roots[1].as.i);
      if (roots[1].tag != CA_INT_T && roots[1].tag != CA_BIG_T)
        ca_error(rt, "TypeError", "enumerate start must be an integer");
      if (!rt->error)
        roots[2] = ca_container(rt, CA_LIST_T, 2, roots);
    } else {
      roots[2] = ca_container(rt, CA_LIST_T, 0, NULL);
      for (int i = 0; i < n && !rt->error; i++) {
        roots[0] = ca_iter(rt, args[i]);
        if (!rt->error)
          ca_append(rt, roots[2], roots[0]);
      }
    }
    CaValue result =
        rt->error ? CA_UNBOUND
                  : ca_native_object(rt, kind == 31 ? CA_F_ENUMERATE : CA_F_ZIP,
                                     roots[2], NULL, 0);
    ca_pop(rt, &frame);
    return result;
  }
  if (kind == 26) {
    if (n < 1 || n > 2 || nkw)
      goto arguments;
    CaValue roots[] = {n == 2 ? args[1] : ca_int(0), ca_iter(rt, args[0]),
                       CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 3);
    if (roots[0].tag == CA_STR_T || roots[0].tag == CA_BYTES_T)
      ca_error(rt, "TypeError", "sum cannot concatenate strings or bytes");
    if (!rt->error)
      while (ca_next(rt, roots[1], &roots[2]) > 0 && !rt->error) {
        roots[0] = ca_binary(rt, roots[0], roots[2], '+');
        if (rt->error)
          break;
        ca_safepoint(rt);
      }
    CaValue result = roots[0];
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : result;
  }
  if (kind == 27 || kind == 28) {
    if (!n)
      goto arguments;
    CaValue key = CA_NONE, empty = CA_UNBOUND;
    for (int i = 0; i < nkw; i++) {
      if (!strcmp(keys[i], "key"))
        key = args[n + i];
      else if (!strcmp(keys[i], "default") && n == 1)
        empty = args[n + i];
      else
        goto arguments;
    }
    CaValue roots[] = {CA_UNBOUND, CA_UNBOUND, CA_UNBOUND, CA_UNBOUND, key};
    CaFrame frame;
    ca_push(rt, &frame, roots, 5);
    if (n == 1)
      roots[2] = ca_iter(rt, args[0]);
    else
      roots[2] = ca_iter(rt, ca_container(rt, CA_TUPLE_T, (size_t)n, args));
    if (!rt->error)
      while (ca_next(rt, roots[2], &roots[3]) > 0 && !rt->error) {
        CaValue k = key.tag == CA_NONE_T
                        ? roots[3]
                        : ca_call(rt, key, 1, 0, &roots[3], NULL);
        if (rt->error)
          break;
        int take = roots[0].tag == CA_UNBOUND_T;
        if (!take) {
          CaValue cmp = ca_compare(rt, k, roots[1], kind == 27 ? "lt" : "gt");
          take = !rt->error && cmp.as.i;
        }
        if (take) {
          roots[0] = roots[3];
          roots[1] = k;
        }
        ca_safepoint(rt);
      }
    CaValue result = roots[0];
    if (!rt->error && result.tag == CA_UNBOUND_T) {
      if (empty.tag == CA_UNBOUND_T)
        ca_error(rt, "ValueError", "min/max arg is an empty sequence");
      else
        result = empty;
    }
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : result;
  }
  if (kind == 29) {
    if (n != 1 || nkw)
      goto arguments;
    if (args[0].tag == CA_FLOAT_T)
      return ca_float(fabs(args[0].as.f));
    if (args[0].tag == CA_INT_T || args[0].tag == CA_BOOL_T ||
        args[0].tag == CA_BIG_T)
      return ca_integer_compare(args[0], ca_int(0)) < 0 ? ca_negate(rt, args[0])
                                                        : args[0];
    ca_error(rt, "TypeError", "abs expects a number");
    return CA_UNBOUND;
  }
  if (kind == 33) {
    if (n != 1 || nkw)
      goto arguments;
    return ca_iter(rt, args[0]);
  }
  if (kind == 34) {
    if (n < 1 || n > 2 || nkw)
      goto arguments;
    CaValue v = CA_UNBOUND;
    int yes = ca_next(rt, args[0], &v);
    if (rt->error)
      return CA_UNBOUND;
    if (yes)
      return v;
    if (n == 2)
      return args[1];
    ca_raise(rt, ca_exception_new(rt, ca_type(CA_T_STOPITERATION), 0, NULL),
             CA_NONE, 0);
    return CA_UNBOUND;
  }
  if (kind == 35) {
    if (n != 1 || nkw)
      goto arguments;
    return ca_bool(
        args[0].tag == CA_FUNCTION_T || args[0].tag == CA_CLOSURE_T ||
        args[0].tag == CA_BOUND_T || args[0].tag == CA_METHOD_T ||
        args[0].tag == CA_CLASS_T || args[0].tag == CA_TYPE_T ||
        (args[0].tag == CA_INSTANCE_T &&
         ca_try_special(rt, args[0], "__call__").tag != CA_UNBOUND_T));
  }
  ca_error(rt, "NotImplementedError",
           "native builtin overload is not implemented");
  return CA_UNBOUND;
arguments:
  ca_error(rt, "TypeError", "invalid arguments to native builtin");
  return CA_UNBOUND;
}
