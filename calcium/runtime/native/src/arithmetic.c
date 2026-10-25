#include "standard.h"
static int integer(CaValue v) {
  return v.tag == CA_INT_T || v.tag == CA_BOOL_T || v.tag == CA_BIG_T;
}
CaValue ca_binary_op(CaRuntime *rt, CaValue a, CaValue b, const char *op) {
  if (!strcmp(op, "or") || !strcmp(op, "and") || !strcmp(op, "xor") ||
      !strcmp(op, "lshift") || !strcmp(op, "rshift"))
    return ca_integer_bitwise(rt, a, b, op);
  if (!strcmp(op, "add"))
    return ca_binary(rt, a, b, '+');
  if (!strcmp(op, "subtract"))
    return ca_binary(rt, a, b, '-');
  if (!strcmp(op, "multiply"))
    return ca_binary(rt, a, b, '*');
  if (integer(a) && integer(b) && !strcmp(op, "true_divide"))
    return ca_integer_true_divide(rt, a, b);
  if (integer(a) && integer(b) &&
      (!strcmp(op, "floor_divide") || !strcmp(op, "remainder"))) {
    if (!ca_integer_compare(b, ca_int(0))) {
      ca_error(rt, "ZeroDivisionError", "integer division or modulo by zero");
      return CA_UNBOUND;
    }
    int negative_a = ca_integer_compare(a, ca_int(0)) < 0,
        negative_b = ca_integer_compare(b, ca_int(0)) < 0;
    CaValue x = negative_a ? ca_negate(rt, a) : a,
            y = negative_b ? ca_negate(rt, b) : b,
            q = ca_positive_divide(rt, x, y);
    CaValue product = ca_numeric(rt, q, y, '*'),
            remainder = ca_numeric(rt, x, product, '-');
    if (rt->error)
      return CA_UNBOUND;
    int nonzero = ca_integer_compare(remainder, ca_int(0)) != 0;
    if (negative_a != negative_b) {
      if (nonzero) {
        q = ca_numeric(rt, q, ca_int(1), '+');
        remainder = ca_numeric(rt, y, remainder, '-');
      }
      q = ca_negate(rt, q);
    }
    if (negative_b)
      remainder = ca_negate(rt, remainder);
    return !strcmp(op, "floor_divide") ? q : remainder;
  }
  if (integer(a) && integer(b) && !strcmp(op, "power") &&
      ca_integer_compare(b, ca_int(0)) >= 0) {
    if (b.tag == CA_BIG_T) {
      if (ca_integer_compare(a, ca_int(0)) == 0)
        return ca_int(0);
      if (ca_integer_compare(a, ca_int(1)) == 0)
        return ca_int(1);
      if (ca_integer_compare(a, ca_int(-1)) == 0)
        return ca_int((((CaBig *)b.as.obj)->limbs[0] & 1) ? -1 : 1);
      ca_error(rt, "OverflowError", "native exponent exceeds platform size");
      return CA_UNBOUND;
    }
    uint64_t n = (uint64_t)b.as.i;
    CaValue roots[] = {a, ca_int(1)};
    CaFrame frame;
    ca_push(rt, &frame, roots, 2);
    while (n && !rt->error) {
      if (n & 1)
        roots[1] = ca_numeric(rt, roots[1], roots[0], '*');
      n >>= 1;
      if (n)
        roots[0] = ca_numeric(rt, roots[0], roots[0], '*');
      if (rt->error)
        break;
      ca_safepoint(rt);
    }
    CaValue result = roots[1];
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : result;
  }
  if (!strcmp(op, "true_divide") && a.tag == CA_FOREIGN_T &&
      ((CaForeign *)a.as.obj)->kind == CA_F_PATH) {
    CaValue values[] = {a, b};
    return ca_io_call(rt, CA_T_PATH, 2, 0, values, NULL);
  }
  double x = ca_as_double(rt, a), y = ca_as_double(rt, b);
  if (rt->error)
    return CA_UNBOUND;
  if (!strcmp(op, "power")) {
    if (x == 0 && y < 0) {
      ca_error(rt, "ZeroDivisionError",
               "zero cannot be raised to a negative power");
      return CA_UNBOUND;
    }
    if (x < 0 && isfinite(y) && floor(y) != y) {
      ca_error(rt, "NotImplementedError",
               "complex arithmetic is not implemented");
      return CA_UNBOUND;
    }
    double result = pow(x, y);
    if (isinf(result) && isfinite(x) && isfinite(y)) {
      ca_error(rt, "OverflowError", "numeric result out of range");
      return CA_UNBOUND;
    }
    return ca_float(result);
  }
  if (y == 0) {
    ca_error(rt, "ZeroDivisionError", "division or modulo by zero");
    return CA_UNBOUND;
  }
  if (!strcmp(op, "true_divide"))
    return ca_float(x / y);
  if (!strcmp(op, "floor_divide") || !strcmp(op, "remainder")) {
    double remainder = fmod(x, y), div = (x - remainder) / y;
    if (remainder) {
      if ((remainder < 0) != (y < 0)) {
        remainder += y;
        div -= 1;
      }
    } else
      remainder = copysign(0, y);
    double quotient;
    if (div) {
      quotient = floor(div);
      if (div - quotient > 0.5)
        quotient += 1;
    } else
      quotient = copysign(0, x / y);
    return ca_float(!strcmp(op, "floor_divide") ? quotient : remainder);
  }
  ca_error(rt, "NotImplementedError",
           "native arithmetic operator is not implemented");
  return CA_UNBOUND;
}
