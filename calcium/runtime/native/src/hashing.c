#include "standard.h"
#include <errno.h>
#include <sys/random.h>
#define PRIME UINT64_C(2305843009213693951)
static uint64_t rotate(uint64_t x, unsigned n) {
  return (x << n) | (x >> (64 - n));
}
static uint64_t little(const unsigned char *p) {
  uint64_t x = 0;
  for (unsigned i = 0; i < 8; i++)
    x |= (uint64_t)p[i] << (8 * i);
  return x;
}
static uint64_t string_hash(CaRuntime *rt, const unsigned char *text,
                            size_t length) {
  static uint64_t secret[2];
  static int ready = 0;
  if (!length)
    return 0;
  if (!ready) {
    size_t at = 0;
    while (at < sizeof secret) {
      ssize_t n =
          getrandom((unsigned char *)secret + at, sizeof secret - at, 0);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        ca_native_oserror(rt, errno, NULL);
        return 0;
      }
      at += (size_t)n;
    }
    ready = 1;
  }
  uint64_t v0 = UINT64_C(0x736f6d6570736575) ^ secret[0],
           v1 = UINT64_C(0x646f72616e646f6d) ^ secret[1],
           v2 = UINT64_C(0x6c7967656e657261) ^ secret[0],
           v3 = UINT64_C(0x7465646279746573) ^ secret[1];
#define ROUND()                                                                \
  do {                                                                         \
    v0 += v1;                                                                  \
    v1 = rotate(v1, 13);                                                       \
    v1 ^= v0;                                                                  \
    v0 = rotate(v0, 32);                                                       \
    v2 += v3;                                                                  \
    v3 = rotate(v3, 16);                                                       \
    v3 ^= v2;                                                                  \
    v0 += v3;                                                                  \
    v3 = rotate(v3, 21);                                                       \
    v3 ^= v0;                                                                  \
    v2 += v1;                                                                  \
    v1 = rotate(v1, 17);                                                       \
    v1 ^= v2;                                                                  \
    v2 = rotate(v2, 32);                                                       \
  } while (0)
  size_t at = 0;
  while (length - at >= 8) {
    uint64_t m = little(text + at);
    v3 ^= m;
    ROUND();
    ROUND();
    v0 ^= m;
    at += 8;
  }
  uint64_t last = (uint64_t)length << 56;
  for (unsigned i = 0; at < length; at++, i++)
    last |= (uint64_t)text[at] << (8 * i);
  v3 ^= last;
  ROUND();
  ROUND();
  v0 ^= last;
  v2 ^= 255;
  ROUND();
  ROUND();
  ROUND();
  ROUND();
#undef ROUND
  return v0 ^ v1 ^ v2 ^ v3;
}
static CaValue signed_hash(uint64_t magnitude, int sign) {
  int64_t value = (int64_t)magnitude;
  if (sign < 0)
    value = -value;
  return ca_int(value == -1 ? -2 : value);
}
CaValue ca_hash_value(CaRuntime *rt, CaValue value) {
  if (value.tag == CA_INT_T || value.tag == CA_BOOL_T) {
    uint64_t n =
        value.as.i < 0 ? 0 - (uint64_t)value.as.i : (uint64_t)value.as.i;
    return signed_hash(n % PRIME, value.as.i < 0 ? -1 : 1);
  }
  if (value.tag == CA_BIG_T) {
    CaBig *b = (CaBig *)value.as.obj;
    uint64_t x = 0;
    for (size_t i = b->count; i--;) {
      x = ((x << 30) & PRIME) | (x >> 31);
      x += b->limbs[i];
      if (x >= PRIME)
        x -= PRIME;
    }
    return signed_hash(x, b->sign);
  }
  if (value.tag == CA_FLOAT_T) {
    double n = value.as.f;
    if (isinf(n))
      return ca_int(n < 0 ? -314159 : 314159);
    if (isnan(n)) {
      CaValue id = ca_identity(rt, value);
      return id.tag == CA_INT_T ? ca_int(id.as.i >> 4) : id;
    }
    int e, sign = n < 0 ? -1 : 1;
    double m = frexp(fabs(n), &e);
    uint64_t x = 0;
    while (m) {
      x = ((x << 28) & PRIME) | (x >> 33);
      m *= 268435456.0;
      e -= 28;
      uint64_t y = (uint64_t)m;
      m -= (double)y;
      x += y;
      if (x >= PRIME)
        x -= PRIME;
    }
    unsigned exponent =
        e >= 0 ? (unsigned)(e % 61) : 60 - (unsigned)((-1 - e) % 61);
    if (exponent)
      x = ((x << exponent) & PRIME) | (x >> (61 - exponent));
    return signed_hash(x, sign);
  }
  if (value.tag == CA_STR_T || value.tag == CA_BYTES_T) {
    CaString *s = (CaString *)value.as.obj;
    uint64_t x = string_hash(rt, (unsigned char *)s->data, s->length);
    return rt->error ? CA_UNBOUND
                     : ca_int((int64_t)(x == UINT64_MAX ? UINT64_MAX - 1 : x));
  }
  if (value.tag == CA_LIST_T || value.tag == CA_DICT_T ||
      value.tag == CA_SET_T) {
    ca_error(rt, "TypeError", "unhashable container");
    return CA_UNBOUND;
  }
  if (value.tag == CA_TUPLE_T) {
    if (!ca_enter(rt))
      return CA_UNBOUND;
    CaFrame frame;
    ca_push(rt, &frame, &value, 1);
    CaContainer *c = (CaContainer *)value.as.obj;
    uint64_t acc = UINT64_C(2870177450012600261);
    for (size_t i = 0; i < c->length && !rt->error; i++) {
      CaValue h = ca_hash_value(rt, c->items[i]);
      if (rt->error)
        break;
      acc += (uint64_t)h.as.i * UINT64_C(14029467366897019727);
      acc = rotate(acc, 31);
      acc *= UINT64_C(11400714785074694791);
    }
    acc ^= 0;
    acc += c->length ^ (UINT64_C(2870177450012600261) ^ UINT64_C(3527539));
    ca_pop(rt, &frame);
    ca_leave(rt);
    return rt->error ? CA_UNBOUND
                     : ca_int(acc == UINT64_MAX ? 1546275796 : (int64_t)acc);
  }
  if (value.tag == CA_SLICE_T) {
    CaSlice *slice = (CaSlice *)value.as.obj;
    CaValue tuple = ca_container(
        rt, CA_TUPLE_T, 3, (CaValue[]){slice->start, slice->stop, slice->step});
    return rt->error ? CA_UNBOUND : ca_hash_value(rt, tuple);
  }
  if (value.tag == CA_RANGE_T) {
    CaRange *r = (CaRange *)value.as.obj;
    CaValue length = ca_range_length(rt, r),
            values[] = {
                length,
                ca_integer_compare(length, ca_int(0)) ? r->start : CA_NONE,
                ca_integer_compare(length, ca_int(1)) > 0 ? r->step : CA_NONE};
    CaValue tuple = ca_container(rt, CA_TUPLE_T, 3, values);
    return rt->error ? CA_UNBOUND : ca_hash_value(rt, tuple);
  }
  if (value.tag == CA_INSTANCE_T) {
    CaValue roots[] = {value, ca_try_special(rt, value, "__hash__"),
                       CA_UNBOUND};
    CaFrame f;
    ca_push(rt, &f, roots, 3);
    if (roots[1].tag == CA_NONE_T ||
        (roots[1].tag == CA_UNBOUND_T &&
         (((CaClass *)((CaInstance *)value.as.obj)->type.as.obj)->dataclass &
          8)))
      ca_error(rt, "TypeError", "unhashable instance");
    else if (roots[1].tag != CA_UNBOUND_T) {
      roots[2] = ca_call(rt, roots[1], 0, 0, NULL, NULL);
      if (!rt->error) {
        if (roots[2].tag != CA_INT_T && roots[2].tag != CA_BIG_T &&
            roots[2].tag != CA_BOOL_T)
          ca_error(rt, "TypeError", "__hash__ must return an integer");
        else
          roots[2] = ca_hash_value(rt, roots[2]);
      }
      CaValue result = roots[2];
      ca_pop(rt, &f);
      return rt->error ? CA_UNBOUND : result;
    }
    ca_pop(rt, &f);
    if (rt->error)
      return CA_UNBOUND;
  }
  if (value.tag == CA_FOREIGN_T) {
    CaForeign *f = (CaForeign *)value.as.obj;
    if (f->kind == CA_F_PATH || f->kind == CA_F_UUID ||
        f->kind == CA_F_DATETIME)
      return ca_hash_value(rt, f->state);
  }
  if (value.tag == CA_METHOD_T) {
    CaMethod *m = (CaMethod *)value.as.obj;
    CaValue id = ca_identity(rt, m->receiver);
    return rt->error ? CA_UNBOUND : ca_int((id.as.i >> 4) ^ m->method);
  }
  CaValue id = ca_identity(rt, value);
  return id.tag == CA_INT_T ? ca_int((id.as.i >> 4) == -1 ? -2 : (id.as.i >> 4))
                            : ca_hash_value(rt, id);
}
