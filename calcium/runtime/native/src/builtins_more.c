#define _GNU_SOURCE
#include "standard.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
static const CaFunction definitions[] = {
#define BUILTIN(name, id) {name, 0, 0, NULL, NULL, id}
    BUILTIN("any", B_ANY),
    BUILTIN("all", B_ALL),
    BUILTIN("bin", B_BIN),
    BUILTIN("hex", B_HEX),
    BUILTIN("oct", B_OCT),
    BUILTIN("chr", B_CHR),
    BUILTIN("ord", B_ORD),
    BUILTIN("pow", B_POW),
    BUILTIN("divmod", B_DIVMOD),
    BUILTIN("round", B_ROUND),
    BUILTIN("map", B_MAP),
    BUILTIN("filter", B_FILTER),
    BUILTIN("reversed", B_REVERSED),
    BUILTIN("open", B_OPEN),
    BUILTIN("input", B_INPUT),
    BUILTIN("exit", B_EXIT),
    BUILTIN("quit", B_QUIT),
    BUILTIN("format", B_FORMAT),
    BUILTIN("delattr", B_DELATTR),
    BUILTIN("vars", B_VARS),
    BUILTIN("id", B_ID),
    BUILTIN("hash", B_HASH),
    BUILTIN("slice", B_SLICE),
    BUILTIN("__call_iter", B_CALL_ITER),
    BUILTIN("readline", 430),
    BUILTIN("readlines", 431),
    BUILTIN("writelines", 432),
    BUILTIN("seek", 433),
    BUILTIN("tell", 434),
#undef BUILTIN
};
CaValue ca_builtin_more_lookup(const char *name) {
  if (!strcmp(name, "slice"))
    return ca_type(CA_T_SLICE);
  if (!strcmp(name, "map"))
    return ca_type(CA_T_MAP);
  if (!strcmp(name, "filter"))
    return ca_type(CA_T_FILTER);
  if (!strcmp(name, "reversed"))
    return ca_type(CA_T_REVERSED);
  for (size_t i = 0; i < sizeof definitions / sizeof definitions[0]; i++)
    if (definitions[i].builtin <= B_SLICE && !strcmp(name, definitions[i].name))
      return ca_function(definitions + i);
  return CA_UNBOUND;
}
CaValue ca_more_method(CaRuntime *rt, CaValue receiver, int id) {
  for (size_t i = 0; i < sizeof definitions / sizeof definitions[0]; i++)
    if (definitions[i].builtin == id)
      return ca_bound(rt, receiver, ca_function(definitions + i));
  return CA_UNBOUND;
}
static int integer(CaValue v) {
  return v.tag == CA_INT_T || v.tag == CA_BIG_T || v.tag == CA_BOOL_T;
}
CaValue ca_builtin_index(CaRuntime *rt, CaValue value) {
  if (integer(value))
    return value.tag == CA_BOOL_T ? ca_int(value.as.i) : value;
  CaValue roots[] = {value, CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 2);
  roots[1] = ca_try_special(rt, value, "__index__");
  if (roots[1].tag == CA_UNBOUND_T) {
    if (!rt->error)
      ca_error(rt, "TypeError", "object cannot be interpreted as an integer");
  } else {
    roots[1] = ca_call(rt, roots[1], 0, 0, NULL, NULL);
    if (!rt->error && !integer(roots[1]))
      ca_error(rt, "TypeError", "__index__ returned a non-integer");
  }
  CaValue result = roots[1];
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : result;
}
static CaValue power(CaRuntime *rt, CaValue base, CaValue exponent,
                     CaValue modulus) {
  if (modulus.tag == CA_NONE_T)
    return ca_binary_op(rt, base, exponent, "power");
  if (!integer(base) || !integer(exponent) || !integer(modulus)) {
    ca_error(rt, "TypeError", "pow with a modulus requires integer arguments");
    return CA_UNBOUND;
  }
  if (!ca_integer_compare(modulus, ca_int(0))) {
    ca_error(rt, "ValueError", "pow() modulus cannot be zero");
    return CA_UNBOUND;
  }
  CaValue r[9] = {base,       exponent,   modulus,    ca_int(1), CA_UNBOUND,
                  CA_UNBOUND, CA_UNBOUND, CA_UNBOUND, CA_UNBOUND};
  CaFrame f;
  ca_push(rt, &f, r, 9);
  if (ca_integer_compare(r[1], ca_int(0)) < 0) {
    r[4] = ca_integer_compare(r[2], ca_int(0)) < 0 ? ca_negate(rt, r[2]) : r[2];
    r[5] = ca_binary_op(rt, r[0], r[4], "remainder");
    r[6] = ca_int(0);
    r[7] = ca_int(1);
    while (!rt->error && ca_integer_compare(r[5], ca_int(0))) {
      r[8] = ca_binary_op(rt, r[4], r[5], "floor_divide");
      CaValue next = ca_numeric(rt, r[4], ca_numeric(rt, r[8], r[5], '*'), '-');
      r[4] = r[5];
      r[5] = next;
      next = ca_numeric(rt, r[6], ca_numeric(rt, r[8], r[7], '*'), '-');
      r[6] = r[7];
      r[7] = next;
      ca_safepoint(rt);
    }
    if (!rt->error && ca_integer_compare(r[4], ca_int(1)))
      ca_error(rt, "ValueError",
               "base is not invertible for the given modulus");
    if (!rt->error) {
      r[0] = ca_binary_op(rt, r[6], r[2], "remainder");
      r[1] = ca_negate(rt, r[1]);
    }
  }
  if (!rt->error) {
    r[0] = ca_binary_op(rt, r[0], r[2], "remainder");
    r[3] = ca_binary_op(rt, r[3], r[2], "remainder");
  }
  while (!rt->error && ca_integer_compare(r[1], ca_int(0)) > 0) {
    r[4] = ca_integer_bitwise(rt, r[1], ca_int(1), "and");
    if (!rt->error && ca_integer_compare(r[4], ca_int(0)))
      r[3] =
          ca_binary_op(rt, ca_numeric(rt, r[3], r[0], '*'), r[2], "remainder");
    r[1] = ca_integer_bitwise(rt, r[1], ca_int(1), "rshift");
    if (!rt->error && ca_integer_compare(r[1], ca_int(0)))
      r[0] =
          ca_binary_op(rt, ca_numeric(rt, r[0], r[0], '*'), r[2], "remainder");
    ca_safepoint(rt);
  }
  CaValue result = r[3];
  ca_pop(rt, &f);
  return rt->error ? CA_UNBOUND : result;
}
static CaValue nearest(CaRuntime *rt, CaValue numerator, CaValue denominator) {
  CaValue r[] = {numerator, denominator, CA_UNBOUND, CA_UNBOUND};
  CaFrame f;
  ca_push(rt, &f, r, 4);
  r[2] = ca_binary_op(rt, r[0], r[1], "floor_divide");
  r[3] = ca_numeric(rt, r[0], ca_numeric(rt, r[2], r[1], '*'), '-');
  r[3] = ca_numeric(rt, r[3], ca_int(2), '*');
  if (!rt->error) {
    int cmp = ca_integer_compare(r[3], r[1]);
    CaValue odd = ca_integer_bitwise(rt, r[2], ca_int(1), "and");
    if (cmp > 0 || (!cmp && ca_integer_compare(odd, ca_int(0))))
      r[2] = ca_numeric(rt, r[2], ca_int(1), '+');
  }
  CaValue result = r[2];
  ca_pop(rt, &f);
  return rt->error ? CA_UNBOUND : result;
}
static CaValue rounding(CaRuntime *rt, CaValue value, CaValue ndigits) {
  if (!integer(value) && value.tag != CA_FLOAT_T) {
    CaValue method = ca_try_special(rt, value, "__round__");
    if (method.tag != CA_UNBOUND_T)
      return ca_call(rt, method, ndigits.tag == CA_NONE_T ? 0 : 1, 0, &ndigits,
                     NULL);
    ca_error(rt, "TypeError", "type does not define __round__");
    return CA_UNBOUND;
  }
  CaValue r[] = {value, ndigits, ca_int(1), CA_UNBOUND, CA_UNBOUND, CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, r, 6);
  long digits = 0;
  if (ndigits.tag != CA_NONE_T) {
    r[1] = ca_builtin_index(rt, ndigits);
    if (rt->error)
      goto done;
    if (r[1].tag == CA_BIG_T)
      digits = ((CaBig *)r[1].as.obj)->sign < 0 ? LONG_MIN : LONG_MAX;
    else
      digits = r[1].as.i;
  }
  if (integer(value)) {
    if (ndigits.tag == CA_NONE_T || digits >= 0) {
      r[5] = value.tag == CA_BOOL_T ? ca_int(value.as.i) : value;
      goto done;
    }
    size_t bits =
        value.tag == CA_BIG_T ? ((CaBig *)value.as.obj)->count * 30 : 64;
    if (digits == LONG_MIN || (uint64_t)(-digits) > bits + 1) {
      r[5] = ca_int(0);
      goto done;
    }
    r[2] = ca_binary_op(rt, ca_int(10), ca_int(-digits), "power");
    r[3] =
        ca_integer_compare(value, ca_int(0)) < 0 ? ca_negate(rt, value) : value;
    r[5] = nearest(rt, r[3], r[2]);
    r[5] = ca_numeric(rt, r[5], r[2], '*');
    if (ca_integer_compare(value, ca_int(0)) < 0)
      r[5] = ca_negate(rt, r[5]);
    goto done;
  }
  if (!isfinite(value.as.f)) {
    if (ndigits.tag != CA_NONE_T) {
      r[5] = value;
      goto done;
    }
    ca_error(rt, isnan(value.as.f) ? "ValueError" : "OverflowError",
             "cannot round a non-finite float to an integer");
    goto done;
  }
  if (ndigits.tag != CA_NONE_T && digits > 323) {
    r[5] = value;
    goto done;
  }
  if (ndigits.tag != CA_NONE_T && digits < -308) {
    r[5] = ca_float(copysign(0, value.as.f));
    goto done;
  }
  int exponent;
  double fraction = frexp(fabs(value.as.f), &exponent);
  r[3] = ca_int((int64_t)ldexp(fraction, 53));
  exponent -= 53;
  if (exponent >= 0)
    r[3] = ca_numeric(
        rt, r[3], ca_integer_bitwise(rt, ca_int(1), ca_int(exponent), "lshift"),
        '*');
  else
    r[2] = ca_integer_bitwise(rt, ca_int(1), ca_int(-exponent), "lshift");
  r[4] = ca_binary_op(rt, ca_int(10), ca_int(digits < 0 ? -digits : digits),
                      "power");
  if (digits >= 0)
    r[3] = ca_numeric(rt, r[3], r[4], '*');
  else
    r[2] = ca_numeric(rt, r[2], r[4], '*');
  r[5] = nearest(rt, r[3], r[2]);
  if (signbit(value.as.f))
    r[5] = ca_negate(rt, r[5]);
  if (ndigits.tag != CA_NONE_T) {
    if (digits < 0)
      r[5] = ca_numeric(rt, r[5], r[4], '*');
    r[5] = ca_integer_true_divide(rt, r[5], digits > 0 ? r[4] : ca_int(1));
    if (!rt->error && r[5].as.f == 0)
      r[5] = ca_float(copysign(0, value.as.f));
  }
done:;
  CaValue result = r[5];
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : result;
}
static CaValue iterator(CaRuntime *rt, int id, int n, int nkw, CaValue *args,
                        const char **keys) {
  if (id == B_REVERSED) {
    if (n != 1 || nkw)
      goto bad;
    CaValue method = ca_try_special(rt, args[0], "__reversed__");
    if (method.tag != CA_UNBOUND_T)
      return ca_call(rt, method, 0, 0, NULL, NULL);
    if (args[0].tag == CA_RANGE_T) {
      CaRange *range = (CaRange *)args[0].as.obj;
      CaValue length = ca_range_length(rt, range);
      CaValue values[] = {
          ca_numeric(rt, range->start,
                     ca_numeric(rt, ca_numeric(rt, length, ca_int(1), '-'),
                                range->step, '*'),
                     '+'),
          ca_numeric(rt, range->start, range->step, '-'),
          ca_negate(rt, range->step)};
      CaValue reversed =
          ca_call(rt, ca_function(&ca_builtin_range), 3, 0, values, NULL);
      return rt->error ? CA_UNBOUND : ca_iter(rt, reversed);
    }
    CaValue length = ca_collection_builtin(rt, 3, 1, 0, args);
    if (rt->error)
      return CA_UNBOUND;
    if (length.tag != CA_INT_T) {
      ca_error(rt, "OverflowError", "sequence length exceeds platform size");
      return CA_UNBOUND;
    }
    CaValue data =
        ca_container(rt, CA_TUPLE_T, 2, (CaValue[]){args[0], length});
    return rt->error ? CA_UNBOUND
                     : ca_native_object(rt, CA_F_REVERSED, data, NULL,
                                        length.as.i - 1);
  }
  if (id == B_CALL_ITER) {
    if (n != 2 || nkw)
      goto bad;
    CaValue callable = ca_extended_call(rt, 35, 1, 0, args, NULL);
    if (rt->error)
      return CA_UNBOUND;
    if (!callable.as.i)
      goto bad;
    CaValue data = ca_container(rt, CA_TUPLE_T, 2, args);
    return rt->error ? CA_UNBOUND
                     : ca_native_object(rt, CA_F_CALL_ITER, data, NULL, 0);
  }
  if (id == B_FILTER) {
    if (n != 2 || nkw)
      goto bad;
    CaValue values[] = {args[0], ca_iter(rt, args[1])};
    CaValue data =
        rt->error ? CA_UNBOUND : ca_container(rt, CA_TUPLE_T, 2, values);
    return rt->error ? CA_UNBOUND
                     : ca_native_object(rt, CA_F_FILTER, data, NULL, 0);
  }
  if (id == B_MAP) {
    if (n < 2 || nkw > 1 || (nkw && strcmp(keys[0], "strict")))
      goto bad;
#if CART_PY_MINOR < 14
    if (nkw)
      goto bad;
#endif
    CaValue roots[] = {ca_container(rt, CA_LIST_T, 0, NULL), args[0],
                       CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 3);
    for (int i = 1; i < n && !rt->error; i++) {
      roots[2] = ca_iter(rt, args[i]);
      if (!rt->error)
        ca_append(rt, roots[0], roots[2]);
    }
    CaValue fields[] = {roots[1], roots[0],
                        ca_bool(nkw ? ca_truth(rt, args[n]) : 0)};
    roots[2] = ca_container(rt, CA_TUPLE_T, 3, fields);
    CaValue result = rt->error
                         ? CA_UNBOUND
                         : ca_native_object(rt, CA_F_MAP, roots[2], NULL, 0);
    ca_pop(rt, &frame);
    return result;
  }
bad:
  ca_error(rt, "TypeError", "invalid iterator builtin arguments");
  return CA_UNBOUND;
}
int ca_builtin_iterator_next(CaRuntime *rt, CaValue value, CaValue *out) {
  CaForeign *f = (CaForeign *)value.as.obj;
  if (f->auxiliary < 0 && f->kind != CA_F_REVERSED)
    return 0;
  CaValue roots[] = {value, CA_UNBOUND, CA_UNBOUND, CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 4);
  int status = 0;
  if (f->kind == CA_F_MAP) {
    CaContainer *iterators = (CaContainer *)ca_capture(f->state, 1).as.obj;
    CaValue *values =
        calloc(iterators->length ? iterators->length : 1, sizeof(CaValue));
    if (!values) {
      ca_error(rt, "MemoryError", "map argument allocation failed");
      goto done;
    }
    CaFrame arguments;
    ca_push(rt, &arguments, values, iterators->length);
    status = 1;
    for (size_t i = 0; i < iterators->length && !rt->error; i++) {
      int next = ca_next(rt, iterators->items[i], values + i);
      if (next <= 0) {
        status = next;
        if (!next && ca_capture(f->state, 2).as.i) {
          int mismatch = i > 0;
          if (!i)
            for (size_t j = 1; j < iterators->length; j++) {
              int more = ca_next(rt, iterators->items[j], &roots[2]);
              if (more < 0)
                break;
              if (more) {
                mismatch = 1;
                break;
              }
            }
          if (!rt->error && mismatch)
            ca_error(rt, "ValueError", "map arguments have different lengths");
        }
        break;
      }
    }
    if (status > 0 && !rt->error)
      roots[1] = ca_call(rt, ca_capture(f->state, 0), (int)iterators->length, 0,
                         values, NULL);
    ca_pop(rt, &arguments);
    free(values);
  } else if (f->kind == CA_F_FILTER) {
    CaValue predicate = ca_capture(f->state, 0), iter = ca_capture(f->state, 1);
    while ((status = ca_next(rt, iter, &roots[1])) > 0 && !rt->error) {
      roots[2] = predicate.tag == CA_NONE_T
                     ? roots[1]
                     : ca_call(rt, predicate, 1, 0, &roots[1], NULL);
      if (rt->error || ca_truth(rt, roots[2]))
        break;
      ca_safepoint(rt);
    }
  } else if (f->kind == CA_F_REVERSED) {
    if (f->auxiliary < 0)
      goto done;
    CaValue sequence = ca_capture(f->state, 0);
    if (sequence.tag == CA_DICT_T) {
      CaContainer *dict = (CaContainer *)sequence.as.obj;
      long expected = ca_capture(f->state, 1).as.i;
      if ((long)dict->length / 2 != expected) {
        ca_error(rt, "RuntimeError",
                 "dictionary changed size during iteration");
        goto done;
      }
      roots[1] = dict->items[(size_t)f->auxiliary * 2];
    } else
      roots[1] = ca_getitem(rt, sequence, ca_int(f->auxiliary));
    f->auxiliary--;
    status = 1;
    if (rt->error &&
        ca_exception_match(rt, rt->pending, ca_type(CA_T_INDEXERROR))) {
      ca_exception_take(rt);
      f->auxiliary = -1;
      status = 0;
    }
  } else if (f->kind == CA_F_CALL_ITER) {
    roots[1] = ca_call(rt, ca_capture(f->state, 0), 0, 0, NULL, NULL);
    status = 1;
    if (!rt->error) {
      CaValue sentinel = ca_capture(f->state, 1);
      roots[2] = ca_same_identity(roots[1], sentinel)
                     ? ca_bool(1)
                     : ca_compare(rt, roots[1], sentinel, "eq");
      if (!rt->error && ca_truth(rt, roots[2]))
        status = 0;
    }
  }
  if (rt->error &&
      ca_exception_match(rt, rt->pending, ca_type(CA_T_STOPITERATION))) {
    ca_exception_take(rt);
    status = 0;
  }
  if (!status && f->kind != CA_F_REVERSED)
    f->auxiliary = -1;
done:
  *out = status > 0 ? roots[1] : CA_UNBOUND;
  ca_pop(rt, &frame);
  return rt->error ? -1 : status;
}
CaValue ca_builtin_more(CaRuntime *rt, int id, int n, int nkw, CaValue *args,
                        const char **keys) {
  if (id >= 430 && id <= 434)
    return ca_file_builtin(rt, id, n, nkw, args, keys);
  if (id == B_OPEN)
    return ca_builtin_open(rt, n, nkw, args, keys);
  if (id == B_MAP || id == B_FILTER || id == B_REVERSED || id == B_CALL_ITER)
    return iterator(rt, id, n, nkw, args, keys);
  if (id == B_POW) {
    CaValue out[3], defaults[] = {CA_NONE, CA_NONE, CA_NONE};
    const char *names[] = {"base", "exp", "mod"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 3, 2, names, defaults,
                             out))
      return CA_UNBOUND;
    return power(rt, out[0], out[1], out[2]);
  }
  if (id == B_ROUND) {
    CaValue out[2], defaults[] = {CA_NONE, CA_NONE};
    const char *names[] = {"number", "ndigits"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 2, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    return rounding(rt, out[0], out[1]);
  }
  if (id == B_EXIT || id == B_QUIT)
    return ca_cli_call(rt, N_SYS_EXIT, n, nkw, args, keys);
  if (id == B_SLICE) {
    if (nkw || n < 1 || n > 3)
      goto bad;
    return ca_slice(rt, n == 1 ? CA_NONE : args[0], n == 1 ? args[0] : args[1],
                    n == 3 ? args[2] : CA_NONE);
  }
  if (id == B_DIVMOD) {
    if (n != 2 || nkw)
      goto bad;
    CaValue roots[] = {args[0], args[1], CA_UNBOUND, CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 4);
    roots[2] = ca_binary_op(rt, args[0], args[1], "floor_divide");
    if (!rt->error)
      roots[3] = ca_binary_op(rt, args[0], args[1], "remainder");
    CaValue result =
        rt->error ? CA_UNBOUND : ca_container(rt, CA_TUPLE_T, 2, roots + 2);
    ca_pop(rt, &frame);
    return result;
  }
  if (id == B_ANY || id == B_ALL) {
    if (n != 1 || nkw)
      goto bad;
    CaValue roots[] = {args[0], CA_UNBOUND, CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 3);
    roots[1] = ca_iter(rt, args[0]);
    int result = id == B_ALL;
    while (!rt->error && ca_next(rt, roots[1], &roots[2]) > 0) {
      int truth = ca_truth(rt, roots[2]);
      if (rt->error)
        break;
      if ((id == B_ANY && truth) || (id == B_ALL && !truth)) {
        result = truth;
        break;
      }
      ca_safepoint(rt);
    }
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : ca_bool(result);
  }
  if (id == B_BIN || id == B_HEX || id == B_OCT) {
    if (n != 1 || nkw)
      goto bad;
    CaValue value = ca_builtin_index(rt, args[0]);
    return rt->error ? CA_UNBOUND
                     : ca_integer_base(rt, value,
                                       id == B_BIN   ? 2
                                       : id == B_HEX ? 16
                                                     : 8);
  }
  if (id == B_CHR) {
    if (n != 1 || nkw)
      goto bad;
    CaValue value = ca_builtin_index(rt, args[0]);
    if (rt->error)
      return CA_UNBOUND;
    if (value.tag == CA_BIG_T || value.as.i < 0 || value.as.i > 0x10ffff) {
      ca_error(rt, "ValueError", "chr() argument outside Unicode range");
      return CA_UNBOUND;
    }
    uint32_t c = (uint32_t)value.as.i;
    if (c >= 0xd800 && c <= 0xdfff) {
      ca_error(rt, "NotImplementedError",
               "native strings do not represent lone surrogates");
      return CA_UNBOUND;
    }
    char b[4];
    size_t length;
    if (c < 128) {
      b[0] = (char)c;
      length = 1;
    } else if (c < 2048) {
      b[0] = (char)(0xc0 | (c >> 6));
      b[1] = (char)(0x80 | (c & 63));
      length = 2;
    } else if (c < 65536) {
      b[0] = (char)(0xe0 | (c >> 12));
      b[1] = (char)(0x80 | ((c >> 6) & 63));
      b[2] = (char)(0x80 | (c & 63));
      length = 3;
    } else {
      b[0] = (char)(0xf0 | (c >> 18));
      b[1] = (char)(0x80 | ((c >> 12) & 63));
      b[2] = (char)(0x80 | ((c >> 6) & 63));
      b[3] = (char)(0x80 | (c & 63));
      length = 4;
    }
    return ca_string(rt, b, length);
  }
  if (id == B_ORD) {
    if (n != 1 || nkw || (args[0].tag != CA_STR_T && args[0].tag != CA_BYTES_T))
      goto bad;
    CaString *s = (CaString *)args[0].as.obj;
    if (!s->length)
      goto bad;
    if (args[0].tag == CA_BYTES_T) {
      if (s->length != 1)
        goto bad;
      return ca_int((unsigned char)s->data[0]);
    }
    size_t length = 1;
    uint32_t c = (unsigned char)s->data[0];
    if (c >= 0xc0) {
      int more = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
      c &= more == 1 ? 31 : more == 2 ? 15 : 7;
      while (more-- && length < s->length)
        c = (c << 6) | ((unsigned char)s->data[length++] & 63);
    }
    if (length != s->length)
      goto bad;
    return ca_int(c);
  }
  if (id == B_FORMAT) {
    if (n < 1 || n > 2 || nkw)
      goto bad;
    CaValue spec = n == 2 ? args[1] : ca_string(rt, "", 0);
    if (spec.tag != CA_STR_T)
      goto bad;
    CaValue method = ca_try_special(rt, args[0], "__format__");
    return method.tag == CA_UNBOUND_T ? ca_format(rt, args[0], spec)
                                      : ca_call(rt, method, 1, 0, &spec, NULL);
  }
  if (id == B_ID) {
    if (n != 1 || nkw)
      goto bad;
    return ca_identity(rt, args[0]);
  }
  if (id == B_HASH) {
    if (n != 1 || nkw)
      goto bad;
    return ca_hash_value(rt, args[0]);
  }
  if (id == B_DELATTR) {
    if (n != 2 || nkw)
      goto bad;
    const char *name = ca_native_text(rt, args[1], 0);
    if (!name)
      return CA_UNBOUND;
    ca_object_delete(rt, args[0], name);
    return rt->error ? CA_UNBOUND : CA_NONE;
  }
  if (id == B_VARS) {
    if (n != 1 || nkw) {
      ca_error(rt, "NotImplementedError",
               "native vars requires an instance argument");
      return CA_UNBOUND;
    }
    if (args[0].tag == CA_INSTANCE_T)
      return ((CaInstance *)args[0].as.obj)->namespace_;
    ca_error(rt, "TypeError", "vars() argument must have a supported __dict__");
    return CA_UNBOUND;
  }
  if (id == B_INPUT) {
    if (n > 1 || nkw)
      goto bad;
    if (n) {
      CaValue prompt = ca_stringify(rt, args[0], 0);
      if (rt->error)
        return CA_UNBOUND;
      CaString *s = (CaString *)prompt.as.obj;
      if (fwrite(s->data, 1, s->length, stdout) != s->length)
        return ca_native_oserror(rt, errno, NULL);
    }
    fflush(stdout);
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, stdin);
    if (length < 0) {
      free(line);
      if (feof(stdin))
        ca_error(rt, "EOFError", "EOF when reading a line");
      else
        ca_native_oserror(rt, errno, NULL);
      return CA_UNBOUND;
    }
    if (length && line[length - 1] == '\n') {
      length--;
    }
    CaValue bytes = ca_bytes(rt, line, (size_t)length);
    free(line);
    return rt->error ? CA_UNBOUND
                     : ca_text_call(rt, N_TEXT_DECODE, 1, 0, &bytes, NULL);
  }
bad:
  ca_error(rt, "TypeError", "invalid arguments to native builtin");
  return CA_UNBOUND;
}
