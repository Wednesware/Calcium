#define _GNU_SOURCE
#include "cart.h"
/* Shortest round-trip decimal through correctly rounded libc formatting.
   Choose Python's fixed/scientific thresholds after finding the precision. */
void ca_write_float(double value, FILE *stream) {
  if (isnan(value)) {
    fputs("nan", stream);
    return;
  }
  if (isinf(value)) {
    fputs(signbit(value) ? "-inf" : "inf", stream);
    return;
  }
  if (value == 0) {
    fputs(signbit(value) ? "-0.0" : "0.0", stream);
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
    fputc('-', stream);
  if (exponent < -4 || exponent >= 16) {
    fputc(digits[0], stream);
    if (count > 1) {
      fputc('.', stream);
      fwrite(digits + 1, 1, (size_t)count - 1, stream);
    }
    fprintf(stream, "e%+03d", exponent);
    return;
  }
  int point = exponent + 1;
  if (point <= 0) {
    fputs("0.", stream);
    for (int i = 0; i < -point; i++)
      fputc('0', stream);
    fwrite(digits, 1, (size_t)count, stream);
  } else {
    for (int i = 0; i < point; i++)
      fputc(i < count ? digits[i] : '0', stream);
    if (point < count) {
      fputc('.', stream);
      fwrite(digits + point, 1, (size_t)(count - point), stream);
    } else
      fputs(".0", stream);
  }
}

void ca_print_float(double v) { ca_write_float(v, stdout); }
