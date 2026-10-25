#include "cart.h"
/* Shortest round-trip decimal through correctly rounded libc formatting.
   Choose Python's fixed/scientific thresholds after finding the precision. */
void ca_print_float(double value) {
  if (isnan(value)) {
    fputs("nan", stdout);
    return;
  }
  if (isinf(value)) {
    fputs(signbit(value) ? "-inf" : "inf", stdout);
    return;
  }
  if (value == 0) {
    fputs(signbit(value) ? "-0.0" : "0.0", stdout);
    return;
  }
  int negative = signbit(value);
  double absolute = fabs(value);
  char text[64];
  int precision;
  for (precision = 1; precision <= 17; precision++) {
    snprintf(text, sizeof text, "%.*e", precision - 1, absolute);
    if (strtod(text, NULL) == absolute)
      break;
  }
  char *e = strchr(text, 'e');
  int exponent = atoi(e + 1);
  char digits[20];
  int count = 0;
  for (char *p = text; p < e; p++)
    if (*p != '.')
      digits[count++] = *p;
  while (count > 1 && digits[count - 1] == '0')
    count--;
  if (negative)
    putchar('-');
  if (exponent < -4 || exponent >= 16) {
    putchar(digits[0]);
    if (count > 1) {
      putchar('.');
      fwrite(digits + 1, 1, (size_t)count - 1, stdout);
    }
    printf("e%+03d", exponent);
    return;
  }
  int point = exponent + 1;
  if (point <= 0) {
    fputs("0.", stdout);
    for (int i = 0; i < -point; i++)
      putchar('0');
    fwrite(digits, 1, (size_t)count, stdout);
  } else {
    for (int i = 0; i < point; i++)
      putchar(i < count ? digits[i] : '0');
    if (point < count) {
      putchar('.');
      fwrite(digits + point, 1, (size_t)(count - point), stdout);
    } else
      fputs(".0", stdout);
  }
}
