#define _GNU_SOURCE
#include "cart.h"
#include <errno.h>
#include <inttypes.h>
#define BASE UINT64_C(1073741824)
#define MASK UINT32_C(1073741823)
typedef struct {
  int sign;
  size_t n;
  const uint32_t *d;
  uint32_t small[3];
} Digits;
static void digits(CaValue v, Digits *d) {
  if (v.tag == CA_BIG_T) {
    CaBig *b = (CaBig *)v.as.obj;
    d->sign = b->sign;
    d->n = b->count;
    d->d = b->limbs;
    return;
  }
  int64_t i = v.as.i;
  uint64_t m = i < 0 ? 0 - (uint64_t)i : (uint64_t)i;
  d->sign = i < 0 ? -1 : i > 0 ? 1 : 0;
  d->n = 0;
  while (m) {
    d->small[d->n++] = (uint32_t)(m & MASK);
    m >>= 30;
  }
  d->d = d->small;
}
static CaBig *allocate_big(CaRuntime *rt, size_t n) {
  if (n > (SIZE_MAX - sizeof(CaBig)) / sizeof(uint32_t)) {
    ca_error(rt, "MemoryError", "integer too large");
    return NULL;
  }
  return ca_allocate(rt, sizeof(CaBig) + n * sizeof(uint32_t), CA_BIG_T);
}
static CaValue finish(CaBig *b) {
  while (b->count && b->limbs[b->count - 1] == 0)
    b->count--;
  if (!b->count)
    return ca_int(0);
  if (b->count <= 3) {
    uint64_t m = 0;
    int fits = 1;
    for (size_t i = b->count; i--;) {
      if (m > (UINT64_MAX >> 30)) {
        fits = 0;
        break;
      }
      m = (m << 30) | b->limbs[i];
    }
    if (fits && m <= (b->sign < 0 ? UINT64_C(9223372036854775808) : INT64_MAX))
      return ca_int(
          b->sign < 0
              ? (m == UINT64_C(9223372036854775808) ? INT64_MIN : -(int64_t)m)
              : (int64_t)m);
  }
  return (CaValue){.tag = CA_BIG_T, .as.obj = (CaObj *)b};
}
CaValue ca_integer(CaRuntime *rt, const char *text) {
  errno = 0;
  char *end;
  intmax_t small = strtoimax(text, &end, 10);
  if (!errno && !*end && small >= INT64_MIN && small <= INT64_MAX)
    return ca_int((int64_t)small);
  int sign = 1;
  if (*text == '-') {
    sign = -1;
    text++;
  } else if (*text == '+')
    text++;
  size_t length = strlen(text);
  CaBig *b = allocate_big(rt, length / 8 + 2);
  if (!b)
    return CA_UNBOUND;
  b->sign = sign;
  for (size_t i = 0; i < length; i++) {
    uint64_t carry = (unsigned)(text[i] - '0');
    for (size_t j = 0; j < b->count; j++) {
      uint64_t v = (uint64_t)b->limbs[j] * 10 + carry;
      b->limbs[j] = (uint32_t)(v & MASK);
      carry = v >> 30;
    }
    if (carry)
      b->limbs[b->count++] = (uint32_t)carry;
  }
  return finish(b);
}
static int magnitude(const Digits *a, const Digits *b) {
  if (a->n != b->n)
    return a->n > b->n ? 1 : -1;
  for (size_t i = a->n; i--;)
    if (a->d[i] != b->d[i])
      return a->d[i] > b->d[i] ? 1 : -1;
  return 0;
}
int ca_integer_compare(CaValue a, CaValue b) {
  Digits x, y;
  digits(a, &x);
  digits(b, &y);
  if (x.sign != y.sign)
    return x.sign > y.sign ? 1 : -1;
  return x.sign * magnitude(&x, &y);
}
static CaValue arithmetic(CaRuntime *rt, CaValue a, CaValue b, char op) {
  Digits x, y;
  digits(a, &x);
  digits(b, &y);
  if (op == '-')
    y.sign = -y.sign;
  if (op == '*') {
    if (!x.sign || !y.sign)
      return ca_int(0);
    if (x.n > SIZE_MAX - y.n) {
      ca_error(rt, "MemoryError", "integer too large");
      return CA_UNBOUND;
    }
    CaBig *out = allocate_big(rt, x.n + y.n);
    if (!out)
      return CA_UNBOUND;
    out->sign = x.sign * y.sign;
    out->count = x.n + y.n;
    for (size_t i = 0; i < x.n; i++) {
      uint64_t carry = 0;
      for (size_t j = 0; j < y.n; j++) {
        uint64_t v = (uint64_t)x.d[i] * y.d[j] + out->limbs[i + j] + carry;
        out->limbs[i + j] = (uint32_t)(v & MASK);
        carry = v >> 30;
      }
      out->limbs[i + y.n] = (uint32_t)carry;
    }
    return finish(out);
  }
  if (x.sign == y.sign) {
    size_t n = x.n > y.n ? x.n : y.n;
    CaBig *out = allocate_big(rt, n + 1);
    if (!out)
      return CA_UNBOUND;
    out->sign = x.sign;
    out->count = n;
    uint64_t carry = 0;
    for (size_t i = 0; i < n; i++) {
      uint64_t v = carry + (i < x.n ? x.d[i] : 0) + (i < y.n ? y.d[i] : 0);
      out->limbs[i] = (uint32_t)(v & MASK);
      carry = v >> 30;
    }
    if (carry)
      out->limbs[out->count++] = (uint32_t)carry;
    return finish(out);
  }
  int cmp = magnitude(&x, &y);
  if (!cmp)
    return ca_int(0);
  Digits *large = cmp > 0 ? &x : &y, *small = cmp > 0 ? &y : &x;
  CaBig *out = allocate_big(rt, large->n);
  if (!out)
    return CA_UNBOUND;
  out->sign = large->sign;
  out->count = large->n;
  int64_t borrow = 0;
  for (size_t i = 0; i < large->n; i++) {
    int64_t v =
        (int64_t)large->d[i] - (i < small->n ? small->d[i] : 0) - borrow;
    borrow = v < 0;
    out->limbs[i] = (uint32_t)(v + (borrow ? (int64_t)BASE : 0));
  }
  return finish(out);
}
static int numeric(CaValue a) {
  return a.tag == CA_INT_T || a.tag == CA_BOOL_T || a.tag == CA_BIG_T ||
         a.tag == CA_FLOAT_T;
}
static int bit(const Digits *d, size_t b) {
  return b / 30 < d->n ? (d->d[b / 30] >> (b % 30)) & 1 : 0;
}
double ca_as_double(CaRuntime *rt, CaValue a) {
  if (!numeric(a)) {
    ca_error(rt, "TypeError", "a real number is required");
    return 0;
  }
  if (a.tag == CA_FLOAT_T)
    return a.as.f;
  if (a.tag != CA_BIG_T)
    return (double)a.as.i;
  Digits d;
  digits(a, &d);
  size_t bits = (d.n - 1) * 30;
  uint32_t high = d.d[d.n - 1];
  while (high) {
    bits++;
    high >>= 1;
  }
  if (bits > 1024) {
    ca_error(rt, "OverflowError", "int too large to convert to float");
    return 0;
  }
  size_t shift = bits > 53 ? bits - 53 : 0;
  uint64_t top = 0;
  for (size_t i = bits; i > shift;)
    top = (top << 1) | (unsigned)bit(&d, --i);
  if (shift && bit(&d, shift - 1)) {
    int sticky = 0;
    for (size_t i = 0; i + 1 < shift; i++)
      if (bit(&d, i)) {
        sticky = 1;
        break;
      }
    if (sticky || (top & 1))
      top++;
  }
  double value = ldexp((double)top, (int)shift);
  if (!isfinite(value))
    ca_error(rt, "OverflowError", "int too large to convert to float");
  return d.sign < 0 ? -value : value;
}
CaValue ca_numeric(CaRuntime *rt, CaValue a, CaValue b, char op) {
  if (!numeric(a) || !numeric(b)) {
    ca_error(rt, "TypeError",
             "unsupported operand types for native arithmetic");
    return CA_UNBOUND;
  }
  if (a.tag == CA_FLOAT_T || b.tag == CA_FLOAT_T) {
    double x = ca_as_double(rt, a), y = ca_as_double(rt, b);
    if (rt->error)
      return CA_UNBOUND;
    return ca_float(op == '+' ? x + y : op == '-' ? x - y : x * y);
  }
  if (a.tag != CA_BIG_T && b.tag != CA_BIG_T) {
    long out;
    int ok = op == '+'   ? ca_long_add((long)a.as.i, (long)b.as.i, &out)
             : op == '-' ? ca_long_subtract((long)a.as.i, (long)b.as.i, &out)
                         : ca_long_multiply((long)a.as.i, (long)b.as.i, &out);
    if (ok)
      return ca_int((int64_t)out);
  }
  return arithmetic(rt, a, b, op);
}
CaValue ca_negate(CaRuntime *rt, CaValue a) {
  return ca_numeric(rt, ca_int(0), a, '-');
}
/* Compare an integer with an IEEE binary64 exactly, without rounding the int.
 */
static int int_float(CaRuntime *rt, CaValue a, double f) {
  if (isinf(f))
    return f > 0 ? -1 : 1;
  double integral = trunc(f);
  int exponent = 0;
  double fraction = frexp(fabs(integral), &exponent);
  uint64_t mantissa = (uint64_t)ldexp(fraction, 53);
  CaValue whole = ca_int(0);
  if (integral != 0) {
    int shift = exponent - 53;
    if (shift < 0)
      mantissa >>= -shift;
    uint32_t limbs[36] = {0};
    size_t count = 0;
    if (shift <= 0) {
      while (mantissa) {
        limbs[count++] = (uint32_t)(mantissa & MASK);
        mantissa >>= 30;
      }
    } else {
      for (int i = 0; i < 53; i++)
        if ((mantissa >> i) & 1) {
          size_t pos = (size_t)(shift + i);
          limbs[pos / 30] |= UINT32_C(1) << (pos % 30);
        }
      count = (size_t)(exponent + 29) / 30;
    }
    CaBig *big = allocate_big(rt, count);
    if (!big)
      return 0;
    big->count = count;
    big->sign = f < 0 ? -1 : 1;
    memcpy(big->limbs, limbs, count * sizeof(uint32_t));
    whole = finish(big);
  }
  int cmp = ca_integer_compare(a, whole);
  if (!cmp && f != integral)
    cmp = f > 0 ? -1 : 1;
  return cmp;
}
int ca_numeric_compare(CaRuntime *rt, CaValue a, CaValue b, int *unordered) {
  *unordered = 0;
  if (!numeric(a) || !numeric(b)) {
    ca_error(rt, "TypeError", "values are not ordered numbers");
    return 0;
  }
  if (a.tag == CA_FLOAT_T && isnan(a.as.f)) {
    *unordered = 1;
    return 0;
  }
  if (b.tag == CA_FLOAT_T && isnan(b.as.f)) {
    *unordered = 1;
    return 0;
  }
  if (a.tag != CA_FLOAT_T && b.tag != CA_FLOAT_T)
    return ca_integer_compare(a, b);
  if (a.tag == CA_FLOAT_T && b.tag == CA_FLOAT_T)
    return (a.as.f > b.as.f) - (a.as.f < b.as.f);
  return a.tag == CA_FLOAT_T ? -int_float(rt, b, a.as.f)
                             : int_float(rt, a, b.as.f);
}
void ca_write_integer(CaRuntime *rt, CaValue a, FILE *stream) {
  if (a.tag != CA_BIG_T) {
    fprintf(stream, "%" PRId64, a.as.i);
    return;
  }
  Digits d;
  digits(a, &d);
  uint32_t *copy = malloc(d.n * sizeof(uint32_t));
  if (d.n > (SIZE_MAX / sizeof(uint32_t) - 1) / 2) {
    ca_error(rt, "MemoryError", "integer display too large");
    free(copy);
    return;
  }
  uint32_t *groups = malloc((d.n * 2 + 1) * sizeof(uint32_t));
  if (!copy || !groups) {
    free(copy);
    free(groups);
    ca_error(rt, "MemoryError", "integer display allocation failed");
    return;
  }
  memcpy(copy, d.d, d.n * sizeof(uint32_t));
  size_t n = d.n, count = 0;
  while (n) {
    uint64_t rem = 0;
    for (size_t i = n; i--;) {
      uint64_t v = (rem << 30) | copy[i];
      copy[i] = (uint32_t)(v / 1000000000);
      rem = v % 1000000000;
    }
    groups[count++] = (uint32_t)rem;
    while (n && !copy[n - 1])
      n--;
  }
  size_t decimal_digits = (count - 1) * 9;
  for (uint32_t top = groups[count - 1]; top; top /= 10)
    decimal_digits++;
  if (decimal_digits > 4300) {
    free(copy);
    free(groups);
    ca_error(rt, "ValueError",
             "integer string conversion exceeds the native 4300-digit limit");
    return;
  }
  if (d.sign < 0)
    fputc('-', stream);
  fprintf(stream, "%u", groups[count - 1]);
  while (--count)
    fprintf(stream, "%09u", groups[count - 1]);
  free(copy);
  free(groups);
}

void ca_print_integer(CaRuntime *rt, CaValue a) {
  ca_write_integer(rt, a, stdout);
}

/* Unsigned long division, with a single-limb fast path and quotient-sized
   shift/subtract work for large ratios of similar bit length. */
static size_t integer_bits(const Digits *x) {
  if (!x->n)
    return 0;
  size_t bits = (x->n - 1) * 30;
  uint32_t top = x->d[x->n - 1];
  while (top) {
    bits++;
    top >>= 1;
  }
  return bits;
}
static CaBig *shift_digits(CaRuntime *rt, const Digits *x, size_t bits) {
  size_t words = bits / 30;
  unsigned small = (unsigned)(bits % 30);
  if (words > SIZE_MAX - x->n - 1) {
    ca_error(rt, "MemoryError", "integer shift too large");
    return NULL;
  }
  CaBig *out = allocate_big(rt, x->n + words + 1);
  if (!out)
    return NULL;
  out->sign = x->sign;
  out->count = x->n + words;
  uint64_t carry = 0;
  for (size_t i = 0; i < x->n; i++) {
    uint64_t value = ((uint64_t)x->d[i] << small) | carry;
    out->limbs[i + words] = (uint32_t)(value & MASK);
    carry = value >> 30;
  }
  if (carry)
    out->limbs[out->count++] = (uint32_t)carry;
  return out;
}
static CaValue positive_shift(CaRuntime *rt, CaValue value, size_t bits) {
  Digits x;
  digits(value, &x);
  if (!x.n)
    return ca_int(0);
  CaBig *out = shift_digits(rt, &x, bits);
  return out ? finish(out) : CA_UNBOUND;
}
CaValue ca_positive_divide(CaRuntime *rt, CaValue a, CaValue b) {
  Digits x, y;
  digits(a, &x);
  digits(b, &y);
  if (x.sign < 0 || y.sign <= 0) {
    ca_error(rt, "SystemError", "invalid positive integer quotient");
    return CA_UNBOUND;
  }
  if (!x.sign || magnitude(&x, &y) < 0)
    return ca_int(0);
  if (a.tag != CA_BIG_T && b.tag != CA_BIG_T)
    return ca_int(a.as.i / b.as.i);
  if (y.n == 1) {
    CaBig *q = allocate_big(rt, x.n);
    if (!q)
      return CA_UNBOUND;
    q->sign = 1;
    q->count = x.n;
    uint64_t remainder = 0;
    for (size_t i = x.n; i--;) {
      uint64_t value = (remainder << 30) | x.d[i];
      q->limbs[i] = (uint32_t)(value / y.d[0]);
      remainder = value % y.d[0];
    }
    return finish(q);
  }
  size_t difference = integer_bits(&x) - integer_bits(&y);
  CaBig *q = allocate_big(rt, difference / 30 + 1),
        *rem = allocate_big(rt, x.n), *den = shift_digits(rt, &y, difference);
  if (!q || !rem || !den)
    return CA_UNBOUND;
  q->sign = 1;
  q->count = difference / 30 + 1;
  rem->sign = 1;
  rem->count = x.n;
  memcpy(rem->limbs, x.d, x.n * sizeof(uint32_t));
  for (size_t bit = difference;; bit--) {
    Digits r = {.sign = 1, .n = rem->count, .d = rem->limbs},
           d = {.sign = 1, .n = den->count, .d = den->limbs};
    if (magnitude(&r, &d) >= 0) {
      int64_t borrow = 0;
      for (size_t i = 0; i < rem->count; i++) {
        int64_t v = (int64_t)rem->limbs[i] -
                    (i < den->count ? den->limbs[i] : 0) - borrow;
        borrow = v < 0;
        rem->limbs[i] = (uint32_t)(v + (borrow ? (int64_t)BASE : 0));
      }
      while (rem->count && !rem->limbs[rem->count - 1])
        rem->count--;
      q->limbs[bit / 30] |= UINT32_C(1) << (bit % 30);
    }
    if (!bit)
      break;
    uint32_t carry = 0;
    for (size_t i = den->count; i--;) {
      uint32_t v = den->limbs[i];
      den->limbs[i] = (v >> 1) | (carry << 29);
      carry = v & 1;
    }
    while (den->count && !den->limbs[den->count - 1])
      den->count--;
  }
  return finish(q);
}
CaValue ca_integer_true_divide(CaRuntime *rt, CaValue a, CaValue b) {
  Digits x, y;
  digits(a, &x);
  digits(b, &y);
  if (!y.sign) {
    ca_error(rt, "ZeroDivisionError", "division by zero");
    return CA_UNBOUND;
  }
  if (a.tag != CA_BIG_T && b.tag != CA_BIG_T &&
      a.as.i >= -INT64_C(9007199254740992) &&
      a.as.i <= INT64_C(9007199254740992) &&
      b.as.i >= -INT64_C(9007199254740992) &&
      b.as.i <= INT64_C(9007199254740992))
    return ca_float((double)a.as.i / (double)b.as.i);
  int negative = (x.sign < 0) != (y.sign < 0);
  if (!x.sign)
    return ca_float(negative ? -0.0 : 0.0);
  size_t xb = integer_bits(&x), yb = integer_bits(&y);
  if (xb > yb && xb - yb > 1024) {
    ca_error(rt, "OverflowError",
             "integer division result too large for float");
    return CA_UNBOUND;
  }
  if (yb > xb && yb - xb > 1076)
    return ca_float(negative ? -0.0 : 0.0);
  int exponent = xb >= yb ? (int)(xb - yb) : -(int)(yb - xb);
  CaValue numerator = x.sign < 0 ? ca_negate(rt, a) : a,
          denominator = y.sign < 0 ? ca_negate(rt, b) : b;
  if (rt->error)
    return CA_UNBOUND;
  CaValue shifted =
      positive_shift(rt, exponent >= 0 ? denominator : numerator,
                     (size_t)(exponent >= 0 ? exponent : -exponent));
  if (rt->error)
    return CA_UNBOUND;
  int cmp = exponent >= 0 ? ca_integer_compare(numerator, shifted)
                          : ca_integer_compare(shifted, denominator);
  if (cmp < 0)
    exponent--;
  if (exponent > 1023) {
    ca_error(rt, "OverflowError",
             "integer division result too large for float");
    return CA_UNBOUND;
  }
  if (exponent < -1075)
    return ca_float(negative ? -0.0 : 0.0);
  int scale = exponent < -1022 ? 1074 : 52 - exponent;
  if (scale >= 0)
    numerator = positive_shift(rt, numerator, (size_t)scale);
  else
    denominator = positive_shift(rt, denominator, (size_t)-scale);
  if (rt->error)
    return CA_UNBOUND;
  CaValue quotient = ca_positive_divide(rt, numerator, denominator);
  if (rt->error)
    return CA_UNBOUND;
  if (quotient.tag != CA_INT_T) {
    ca_error(rt, "SystemError", "invalid native division significand");
    return CA_UNBOUND;
  }
  CaValue product = ca_numeric(rt, quotient, denominator, '*'),
          remainder = ca_numeric(rt, numerator, product, '-'),
          twice = ca_numeric(rt, remainder, ca_int(2), '*');
  if (rt->error)
    return CA_UNBOUND;
  uint64_t significand = (uint64_t)quotient.as.i;
  cmp = ca_integer_compare(twice, denominator);
  if (cmp > 0 || (cmp == 0 && (significand & 1)))
    significand++;
  double value = ldexp((double)significand, -scale);
  if (isinf(value)) {
    ca_error(rt, "OverflowError",
             "integer division result too large for float");
    return CA_UNBOUND;
  }
  return ca_float(negative ? -value : value);
}
CaValue ca_integer_bitwise(CaRuntime *rt, CaValue a, CaValue b,
                           const char *op) {
  if ((a.tag != CA_INT_T && a.tag != CA_BIG_T && a.tag != CA_BOOL_T) ||
      (b.tag != CA_INT_T && b.tag != CA_BIG_T && b.tag != CA_BOOL_T)) {
    ca_error(rt, "TypeError", "bitwise operations require integers");
    return CA_UNBOUND;
  }
  Digits x, y;
  digits(a, &x);
  digits(b, &y);
  if (!strcmp(op, "lshift") || !strcmp(op, "rshift")) {
    if (y.sign < 0) {
      ca_error(rt, "ValueError", "negative shift count");
      return CA_UNBOUND;
    }
    if (!x.sign)
      return ca_int(0);
    int right = !strcmp(op, "rshift");
    if (b.tag == CA_BIG_T || (uint64_t)b.as.i > SIZE_MAX / sizeof(uint32_t)) {
      if (right)
        return ca_int(x.sign < 0 ? -1 : 0);
      ca_error(rt, "OverflowError", "shift count exceeds platform size");
      return CA_UNBOUND;
    }
    size_t shift = (size_t)b.as.i, whole = shift / 30;
    unsigned part = (unsigned)(shift % 30);
    if (right) {
      if (whole >= x.n)
        return ca_int(x.sign < 0 ? -1 : 0);
      CaBig *r = allocate_big(rt, x.n - whole);
      if (!r)
        return CA_UNBOUND;
      r->count = x.n - whole;
      r->sign = x.sign;
      int discarded = 0;
      for (size_t i = 0; i < whole; i++)
        discarded |= x.d[i] != 0;
      if (part)
        discarded |= (x.d[whole] & ((1u << part) - 1)) != 0;
      for (size_t i = 0; i < r->count; i++) {
        uint64_t v = x.d[i + whole];
        if (i + whole + 1 < x.n)
          v |= (uint64_t)x.d[i + whole + 1] << 30;
        r->limbs[i] = (uint32_t)((v >> part) & MASK);
      }
      CaValue value = finish(r);
      return x.sign < 0 && discarded ? ca_numeric(rt, value, ca_int(1), '-')
                                     : value;
    }
    if (whole > SIZE_MAX - x.n - 1) {
      ca_error(rt, "MemoryError", "integer too large");
      return CA_UNBOUND;
    }
    CaBig *r = allocate_big(rt, x.n + whole + 1);
    if (!r)
      return CA_UNBOUND;
    r->sign = x.sign;
    r->count = x.n + whole + 1;
    uint64_t carry = 0;
    for (size_t i = 0; i < x.n; i++) {
      uint64_t v = ((uint64_t)x.d[i] << part) | carry;
      r->limbs[i + whole] = (uint32_t)(v & MASK);
      carry = v >> 30;
    }
    r->limbs[x.n + whole] = (uint32_t)carry;
    return finish(r);
  }
  size_t n = (x.n > y.n ? x.n : y.n) + 1;
  CaBig *r = allocate_big(rt, n);
  if (!r)
    return CA_UNBOUND;
  r->count = n;
  uint64_t cx = x.sign < 0, cy = y.sign < 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t u = i < x.n ? x.d[i] : 0, v = i < y.n ? y.d[i] : 0;
    if (x.sign < 0) {
      u = (~u & MASK) + cx;
      cx = u >> 30;
      u &= MASK;
    }
    if (y.sign < 0) {
      v = (~v & MASK) + cy;
      cy = v >> 30;
      v &= MASK;
    }
    r->limbs[i] = (uint32_t)(!strcmp(op, "or")    ? (u | v)
                             : !strcmp(op, "and") ? (u & v)
                                                  : (u ^ v));
  }
  r->sign = (r->limbs[n - 1] & (1u << 29)) ? -1 : 1;
  if (r->sign < 0) {
    uint64_t carry = 1;
    for (size_t i = 0; i < n; i++) {
      uint64_t v = (~r->limbs[i] & MASK) + carry;
      r->limbs[i] = (uint32_t)(v & MASK);
      carry = v >> 30;
    }
  }
  CaValue value = finish(r);
  return a.tag == CA_BOOL_T && b.tag == CA_BOOL_T ? ca_bool(value.as.i != 0)
                                                  : value;
}
CaValue ca_integer_base(CaRuntime *rt, CaValue value, int base) {
  Digits d;
  digits(value, &d);
  size_t bits = d.n ? 30 * (d.n - 1) : 0;
  if (d.n) {
    uint32_t high = d.d[d.n - 1];
    while (high) {
      bits++;
      high >>= 1;
    }
  }
  unsigned group = base == 2 ? 1 : base == 8 ? 3 : 4;
  size_t count = (bits + group - 1) / group;
  if (!count)
    count = 1;
  if (count > SIZE_MAX - 4) {
    ca_error(rt, "MemoryError", "integer string too large");
    return CA_UNBOUND;
  }
  char *buffer = malloc(count + 4);
  if (!buffer) {
    ca_error(rt, "MemoryError", "integer format allocation failed");
    return CA_UNBOUND;
  }
  size_t at = 0;
  if (d.sign < 0)
    buffer[at++] = '-';
  buffer[at++] = '0';
  buffer[at++] = base == 2 ? 'b' : base == 8 ? 'o' : 'x';
  static const char digits[] = "0123456789abcdef";
  for (size_t i = count; i--;) {
    size_t bit = i * group, index = bit / 30;
    unsigned offset = (unsigned)(bit % 30);
    uint64_t v = index < d.n ? d.d[index] : 0;
    if (index + 1 < d.n)
      v |= (uint64_t)d.d[index + 1] << 30;
    buffer[at++] = digits[(v >> offset) & ((1u << group) - 1)];
  }
  CaValue result = ca_string(rt, buffer, at);
  free(buffer);
  return result;
}
