#define _GNU_SOURCE
#include "standard.h"
#include <ctype.h>
#include <errno.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>
static int entropy(CaRuntime *rt, unsigned char *buffer, size_t length) {
  size_t at = 0;
  while (at < length) {
    ssize_t n = getrandom(buffer + at, length - at, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      ca_native_oserror(rt, errno, NULL);
      return 0;
    }
    if (!n) {
      ca_error(rt, "OSError", "system entropy source returned no data");
      return 0;
    }
    at += (size_t)n;
  }
  return 1;
}
static CaValue date_parts(CaRuntime *rt, time_t seconds, long micros,
                          int aware) {
  struct tm tm;
  if (!(aware ? gmtime_r(&seconds, &tm) : localtime_r(&seconds, &tm)) ||
      tm.tm_year < -1899 || tm.tm_year > 8099) {
    ca_error(rt, "ValueError", "year out of range");
    return CA_UNBOUND;
  }
  char text[64];
  int length = snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d",
                        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                        tm.tm_hour, tm.tm_min, tm.tm_sec);
  if (micros)
    length +=
        snprintf(text + length, sizeof text - (size_t)length, ".%06ld", micros);
  if (aware)
    length += snprintf(text + length, sizeof text - (size_t)length, "+00:00");
  CaValue state = ca_string(rt, text, (size_t)length);
  return rt->error ? CA_UNBOUND
                   : ca_native_object(rt, CA_F_DATETIME, state,
                                      (void *)(uintptr_t)aware,
                                      (long)seconds * 1000000 + micros);
}
static CaValue date(CaRuntime *rt, double timestamp, int aware) {
  if (!isfinite(timestamp) || timestamp < (double)INT64_MIN / 1000000 ||
      timestamp > (double)INT64_MAX / 1000000) {
    ca_error(rt, "OverflowError", "timestamp out of native datetime range");
    return CA_UNBOUND;
  }
  double floor_seconds = floor(timestamp);
  time_t seconds = (time_t)floor_seconds;
  long micros = (long)nearbyint((timestamp - floor_seconds) * 1000000);
  if (micros == 1000000) {
    seconds++;
    micros = 0;
  }
  return date_parts(rt, seconds, micros, aware);
}
static int timezone_kind(CaRuntime *rt, CaValue value) {
  if (value.tag == CA_NONE_T)
    return 0;
  if (value.tag == CA_FOREIGN_T &&
      ((CaForeign *)value.as.obj)->kind == CA_F_UTC)
    return 1;
  ca_error(rt, "NotImplementedError",
           "only UTC and local naive datetime values are implemented");
  return -1;
}
CaValue ca_system_attribute(CaRuntime *rt, CaValue value, const char *name) {
  CaForeign *f = (CaForeign *)value.as.obj;
  if (f->kind == CA_F_UUID) {
    CaString *s = (CaString *)f->state.as.obj;
    char hex[33];
    size_t at = 0;
    for (size_t i = 0; i < s->length; i++)
      if (s->data[i] != '-')
        hex[at++] = s->data[i];
    hex[at] = 0;
    if (!strcmp(name, "hex"))
      return ca_string(rt, hex, 32);
    if (!strcmp(name, "version"))
      return ca_int(hex[12] >= 'a' ? hex[12] - 'a' + 10 : hex[12] - '0');
    if (!strcmp(name, "int")) {
      CaValue args[] = {ca_string(rt, hex, 32), ca_int(16)};
      return rt->error ? CA_UNBOUND
                       : ca_extended_call(rt, CA_T_INT, 2, 0, args, NULL);
    }
    if (!strcmp(name, "bytes")) {
      unsigned char bytes[16];
      for (size_t i = 0; i < 16; i++) {
        int a = hex[i * 2] >= 'a' ? hex[i * 2] - 'a' + 10 : hex[i * 2] - '0',
            b = hex[i * 2 + 1] >= 'a' ? hex[i * 2 + 1] - 'a' + 10
                                      : hex[i * 2 + 1] - '0';
        bytes[i] = (unsigned char)((a << 4) | b);
      }
      return ca_bytes(rt, (const char *)bytes, 16);
    }
  }
  if (f->kind == CA_F_DATETIME) {
    int64_t secs = f->auxiliary / 1000000, micro = f->auxiliary % 1000000;
    if (micro < 0) {
      secs--;
      micro += 1000000;
    }
    time_t seconds = (time_t)secs;
    struct tm tm;
    if (!(f->handle ? gmtime_r(&seconds, &tm) : localtime_r(&seconds, &tm)))
      return ca_native_oserror(rt, errno, NULL);
    const char *names[] = {"year",   "month",  "day",        "hour",
                           "minute", "second", "microsecond"};
    int64_t fields[] = {
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
        tm.tm_min,         tm.tm_sec,     micro};
    for (size_t i = 0; i < 7; i++)
      if (!strcmp(name, names[i]))
        return ca_int(fields[i]);
    if (!strcmp(name, "tzinfo")) {
      if (!f->handle)
        return CA_NONE;
      CaModule *m = ca_standard_find(rt, "datetime");
      ca_module_start(rt, m);
      return ca_attribute(rt, ca_module_value(m), "UTC", 0);
    }
  }
  ca_error(rt, "AttributeError", name);
  return CA_UNBOUND;
}
CaValue ca_system_call(CaRuntime *rt, int kind, int n, int nkw, CaValue *args,
                       const char **keys) {
  CaValue out[8], defaults[] = {CA_NONE, CA_NONE, CA_NONE, CA_NONE,
                                CA_NONE, CA_NONE, CA_NONE, CA_NONE};
  if (kind == N_TIME || kind == N_MONOTONIC) {
    const char *names[] = {""};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 0, 0, names, defaults,
                             out))
      return CA_UNBOUND;
    struct timespec time;
    if (clock_gettime(kind == N_TIME ? CLOCK_REALTIME : CLOCK_MONOTONIC, &time))
      return ca_native_oserror(rt, errno, NULL);
    return ca_float((double)time.tv_sec + (double)time.tv_nsec / 1e9);
  }
  if (kind == N_SLEEP) {
    const char *names[] = {"seconds"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 1, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    double seconds = ca_as_double(rt, out[0]);
    if (rt->error)
      return CA_UNBOUND;
    if (!isfinite(seconds) || seconds < 0) {
      ca_error(rt, "ValueError",
               "sleep length must be non-negative and finite");
      return CA_UNBOUND;
    }
    if (seconds > (double)INT64_MAX) {
      ca_error(rt, "OverflowError", "sleep length too large");
      return CA_UNBOUND;
    }
    struct timespec wait = {(time_t)seconds,
                            (long)((seconds - floor(seconds)) * 1e9)};
    while (nanosleep(&wait, &wait)) {
      if (errno != EINTR)
        return ca_native_oserror(rt, errno, NULL);
    }
    return CA_NONE;
  }
  if (kind == N_ENVIRON_GET) {
    const char *names[] = {"self", "key", "default"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 3, 2, names, defaults,
                             out))
      return CA_UNBOUND;
    const char *key = ca_native_text(rt, out[1], 0);
    if (!key)
      return CA_UNBOUND;
    const char *value = getenv(key);
    return value ? ca_string(rt, value, strlen(value)) : out[2];
  }
  if (kind == N_DATETIME_NOW || kind == N_DATETIME_FROMTIMESTAMP) {
    const char *names[] = {kind == N_DATETIME_NOW ? "tz" : "timestamp", "tz"};
    int total = kind == N_DATETIME_NOW ? 1 : 2,
        required = kind == N_DATETIME_NOW ? 0 : 1;
    if (!ca_native_arguments(rt, n, nkw, args, keys, total, required, names,
                             defaults, out))
      return CA_UNBOUND;
    int aware = timezone_kind(rt, out[kind == N_DATETIME_NOW ? 0 : 1]);
    if (aware < 0)
      return CA_UNBOUND;
    double timestamp;
    if (kind == N_DATETIME_NOW) {
      struct timespec ts;
      if (clock_gettime(CLOCK_REALTIME, &ts))
        return ca_native_oserror(rt, errno, NULL);
      timestamp = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    } else
      timestamp = ca_as_double(rt, out[0]);
    return rt->error ? CA_UNBOUND : date(rt, timestamp, aware);
  }
  if (kind == N_DATETIME_ISOFORMAT) {
    const char *names[] = {"self", "sep", "timespec"};
    defaults[1] = ca_string(rt, "T", 1);
    defaults[2] = ca_string(rt, "auto", 4);
    if (!ca_native_arguments(rt, n, nkw, args, keys, 3, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    CaForeign *f = (CaForeign *)out[0].as.obj;
    const char *spec = ca_native_text(rt, out[2], 0);
    if (!spec)
      return CA_UNBOUND;
    if (out[1].tag != CA_STR_T) {
      ca_error(rt, "TypeError", "separator must be str");
      return CA_UNBOUND;
    }
    CaString *sep = (CaString *)out[1].as.obj;
    size_t points = 0;
    for (size_t i = 0; i < sep->length; i++)
      if (((unsigned char)sep->data[i] & 0xc0) != 0x80)
        points++;
    if (points != 1) {
      ca_error(rt, "TypeError", "separator must be one character");
      return CA_UNBOUND;
    }
    CaString *text = (CaString *)f->state.as.obj;
    char buffer[80];
    size_t at = 0;
    memcpy(buffer, text->data, 10);
    at = 10;
    memcpy(buffer + at, sep->data, sep->length);
    at += sep->length;
    size_t time_length = !strcmp(spec, "hours")     ? 2
                         : !strcmp(spec, "minutes") ? 5
                                                    : 8;
    memcpy(buffer + at, text->data + 11, time_length);
    at += time_length;
    long micros = f->auxiliary % 1000000;
    if (micros < 0)
      micros += 1000000;
    if (!strcmp(spec, "milliseconds"))
      at += (size_t)snprintf(buffer + at, sizeof buffer - at, ".%03ld",
                             micros / 1000);
    else if (!strcmp(spec, "microseconds") || (!strcmp(spec, "auto") && micros))
      at += (size_t)snprintf(buffer + at, sizeof buffer - at, ".%06ld", micros);
    else if (strcmp(spec, "auto") && strcmp(spec, "seconds") &&
             strcmp(spec, "hours") && strcmp(spec, "minutes")) {
      ca_error(rt, "ValueError", "unknown timespec value");
      return CA_UNBOUND;
    }
    if (f->handle) {
      memcpy(buffer + at, "+00:00", 6);
      at += 6;
    }
    return ca_string(rt, buffer, at);
  }
  if (kind == CA_T_DATETIME) {
    const char *names[] = {"year",   "month",  "day",         "hour",
                           "minute", "second", "microsecond", "tzinfo"};
    defaults[3] = defaults[4] = defaults[5] = defaults[6] = ca_int(0);
    if (!ca_native_arguments(rt, n, nkw, args, keys, 8, 3, names, defaults,
                             out))
      return CA_UNBOUND;
    for (int i = 0; i < 7; i++)
      if (out[i].tag != CA_INT_T && out[i].tag != CA_BOOL_T) {
        ca_error(rt, "TypeError", "datetime component must be an integer");
        return CA_UNBOUND;
      }
    long year = out[0].as.i, month = out[1].as.i, day = out[2].as.i,
         hour = out[3].as.i, minute = out[4].as.i, second = out[5].as.i,
         micro = out[6].as.i;
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59 || micro < 0 || micro >= 1000000) {
      ca_error(rt, "ValueError", "datetime component out of range");
      return CA_UNBOUND;
    }
    int aware = timezone_kind(rt, out[7]);
    if (aware < 0)
      return CA_UNBOUND;
    struct tm tm = {.tm_year = (int)year - 1900,
                    .tm_mon = (int)month - 1,
                    .tm_mday = (int)day,
                    .tm_hour = (int)hour,
                    .tm_min = (int)minute,
                    .tm_sec = (int)second,
                    .tm_isdst = -1};
    time_t stamp = aware ? timegm(&tm) : mktime(&tm);
    if (tm.tm_year != year - 1900 || tm.tm_mon != month - 1 ||
        tm.tm_mday != day) {
      ca_error(rt, "ValueError", "day is out of range for month");
      return CA_UNBOUND;
    }
    return date_parts(rt, stamp, micro, aware);
  }
  if (kind == N_UUID4 || kind == N_TOKEN_HEX || kind == N_TOKEN_URLSAFE) {
    size_t count = 16;
    const char *names[] = {"nbytes"};
    if (kind == N_UUID4) {
      if (!ca_native_arguments(rt, n, nkw, args, keys, 0, 0, names, defaults,
                               out))
        return CA_UNBOUND;
    } else {
      defaults[0] = ca_int(32);
      if (!ca_native_arguments(rt, n, nkw, args, keys, 1, 0, names, defaults,
                               out))
        return CA_UNBOUND;
      if (out[0].tag == CA_NONE_T)
        out[0] = ca_int(32);
      if (out[0].tag != CA_INT_T || out[0].as.i < 0) {
        ca_error(rt, "ValueError", "nbytes must be a non-negative integer");
        return CA_UNBOUND;
      }
      count = (size_t)out[0].as.i;
    }
    if (count > SIZE_MAX / 4 - 4) {
      ca_error(rt, "OverflowError", "token length too large");
      return CA_UNBOUND;
    }
    unsigned char *data = malloc(count ? count : 1);
    if (!data) {
      ca_error(rt, "MemoryError", "random buffer allocation failed");
      return CA_UNBOUND;
    }
    if (!entropy(rt, data, count)) {
      free(data);
      return CA_UNBOUND;
    }
    if (kind == N_UUID4) {
      data[6] = (data[6] & 15) | 64;
      data[8] = (data[8] & 63) | 128;
    }
    size_t size = count * 2 + 5;
    if (kind == N_TOKEN_URLSAFE)
      size = (count + 2) / 3 * 4 + 1;
    char *text = malloc(size);
    if (!text) {
      free(data);
      ca_error(rt, "MemoryError", "token allocation failed");
      return CA_UNBOUND;
    }
    size_t at = 0;
    const char *digits = "0123456789abcdef";
    if (kind == N_TOKEN_URLSAFE) {
      const char *alphabet =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
      for (size_t i = 0; i < count; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < count)
          v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < count)
          v |= data[i + 2];
        text[at++] = alphabet[v >> 18];
        text[at++] = alphabet[(v >> 12) & 63];
        if (i + 1 < count)
          text[at++] = alphabet[(v >> 6) & 63];
        if (i + 2 < count)
          text[at++] = alphabet[v & 63];
      }
    } else
      for (size_t i = 0; i < count; i++) {
        if (kind == N_UUID4 && (i == 4 || i == 6 || i == 8 || i == 10))
          text[at++] = '-';
        text[at++] = digits[data[i] >> 4];
        text[at++] = digits[data[i] & 15];
      }
    CaValue value = ca_string(rt, text, at);
    free(text);
    free(data);
    if (kind == N_UUID4 && !rt->error)
      value = ca_native_object(rt, CA_F_UUID, value, NULL, 0);
    return value;
  }
  if (kind == CA_T_UUID) {
    const char *names[] = {"hex"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 1, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    const char *text = ca_native_text(rt, out[0], 0);
    if (!text)
      return CA_UNBOUND;
    if (!strncmp(text, "urn:uuid:", 9))
      text += 9;
    char hex[32];
    size_t at = 0;
    for (size_t i = 0; text[i]; i++) {
      if (text[i] == '-' || text[i] == '{' || text[i] == '}')
        continue;
      if (!isxdigit((unsigned char)text[i]) || at == 32) {
        ca_error(rt, "ValueError", "badly formed hexadecimal UUID string");
        return CA_UNBOUND;
      }
      hex[at++] = (char)tolower((unsigned char)text[i]);
    }
    if (at != 32) {
      ca_error(rt, "ValueError", "badly formed hexadecimal UUID string");
      return CA_UNBOUND;
    }
    char canonical[36];
    at = 0;
    for (size_t i = 0; i < 32; i++) {
      if (i == 8 || i == 12 || i == 16 || i == 20)
        canonical[at++] = '-';
      canonical[at++] = hex[i];
    }
    CaValue value = ca_string(rt, canonical, 36);
    return rt->error ? CA_UNBOUND
                     : ca_native_object(rt, CA_F_UUID, value, NULL, 0);
  }
  ca_error(rt, "NotImplementedError",
           "native system API overload is not implemented");
  return CA_UNBOUND;
}
