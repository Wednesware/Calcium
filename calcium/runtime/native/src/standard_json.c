#define _GNU_SOURCE
#include "standard.h"
#include <ctype.h>
#include <errno.h>
typedef struct {
  CaRuntime *rt;
  CaValue input, *options;
  const char *text;
  size_t length, at;
} Parser;
static void whitespace(Parser *p) {
  while (p->at < p->length &&
         (p->text[p->at] == ' ' || p->text[p->at] == '\t' ||
          p->text[p->at] == '\r' || p->text[p->at] == '\n'))
    p->at++;
}
static CaValue error(Parser *p, const char *message) {
  size_t line = 1, column = 1;
  for (size_t i = 0; i < p->at && i < p->length; i++) {
    if (p->text[i] == '\n') {
      line++;
      column = 1;
    } else if (((unsigned char)p->text[i] & 0xc0) != 0x80)
      column++;
  }
  char text[256];
  snprintf(text, sizeof text, "%s: line %zu column %zu", message, line, column);
  ca_error(p->rt, "json.decoder.JSONDecodeError", text);
  return CA_UNBOUND;
}
static int hex(int c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}
static int escaped_code(Parser *p, uint32_t *out) {
  if (p->length - p->at < 4) {
    error(p, "Invalid unicode escape");
    return 0;
  }
  uint32_t value = 0;
  for (int i = 0; i < 4; i++) {
    int digit = hex((unsigned char)p->text[p->at++]);
    if (digit < 0) {
      error(p, "Invalid unicode escape");
      return 0;
    }
    value = (value << 4) | (unsigned)digit;
  }
  *out = value;
  return 1;
}
static void put_code(FILE *stream, uint32_t c) {
  if (c < 128)
    fputc((int)c, stream);
  else if (c < 2048) {
    fputc(0xc0 | (int)(c >> 6), stream);
    fputc(0x80 | (int)(c & 63), stream);
  } else if (c < 65536) {
    fputc(0xe0 | (int)(c >> 12), stream);
    fputc(0x80 | (int)((c >> 6) & 63), stream);
    fputc(0x80 | (int)(c & 63), stream);
  } else {
    fputc(0xf0 | (int)(c >> 18), stream);
    fputc(0x80 | (int)((c >> 12) & 63), stream);
    fputc(0x80 | (int)((c >> 6) & 63), stream);
    fputc(0x80 | (int)(c & 63), stream);
  }
}
static CaValue string(Parser *p) {
  p->at++;
  char *buffer = NULL;
  size_t length = 0;
  FILE *stream = open_memstream(&buffer, &length);
  if (!stream) {
    ca_error(p->rt, "MemoryError", "JSON string allocation failed");
    return CA_UNBOUND;
  }
  int closed = 0;
  while (p->at < p->length && !p->rt->error) {
    unsigned c = (unsigned char)p->text[p->at++];
    if (c == '"') {
      closed = 1;
      break;
    }
    if (c < 32 && ca_truth(p->rt, p->options[6])) {
      error(p, "Invalid control character");
      break;
    }
    if (c != '\\') {
      fputc((int)c, stream);
      continue;
    }
    if (p->at == p->length) {
      error(p, "Unterminated string");
      break;
    }
    c = (unsigned char)p->text[p->at++];
    switch (c) {
    case '"':
    case '\\':
    case '/':
      fputc((int)c, stream);
      break;
    case 'b':
      fputc('\b', stream);
      break;
    case 'f':
      fputc('\f', stream);
      break;
    case 'n':
      fputc('\n', stream);
      break;
    case 'r':
      fputc('\r', stream);
      break;
    case 't':
      fputc('\t', stream);
      break;
    case 'u': {
      uint32_t code;
      if (!escaped_code(p, &code))
        break;
      if (code >= 0xd800 && code <= 0xdbff) {
        if (p->length - p->at < 6 || p->text[p->at] != '\\' ||
            p->text[p->at + 1] != 'u') {
          ca_error(p->rt, "NotImplementedError",
                   "unpaired surrogate strings are not implemented");
          break;
        }
        p->at += 2;
        uint32_t low;
        if (!escaped_code(p, &low))
          break;
        if (low < 0xdc00 || low > 0xdfff) {
          ca_error(p->rt, "NotImplementedError",
                   "unpaired surrogate strings are not implemented");
          break;
        }
        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
      } else if (code >= 0xdc00 && code <= 0xdfff) {
        ca_error(p->rt, "NotImplementedError",
                 "unpaired surrogate strings are not implemented");
        break;
      }
      put_code(stream, code);
      break;
    }
    default:
      error(p, "Invalid escape");
      break;
    }
  }
  if (!closed && !p->rt->error)
    error(p, "Unterminated string");
  if (fclose(stream) && !p->rt->error)
    ca_error(p->rt, "MemoryError", "JSON string allocation failed");
  CaValue result = p->rt->error ? CA_UNBOUND : ca_string(p->rt, buffer, length);
  free(buffer);
  return result;
}
static CaValue parse(Parser *p) {
  CaRuntime *rt = p->rt;
  if (!ca_enter(rt))
    return CA_UNBOUND;
  CaValue roots[] = {CA_UNBOUND, CA_UNBOUND, CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 3);
  ca_safepoint(rt);
  whitespace(p);
  if (p->at >= p->length) {
    error(p, "Expecting value");
    goto done;
  }
  char c = p->text[p->at];
  if (c == '"') {
    roots[0] = string(p);
    goto done;
  }
  if (c == '[' || c == '{') {
    int object = c == '{', pairs = object && p->options[5].tag != CA_NONE_T;
    char end = object ? '}' : ']';
    p->at++;
    roots[0] =
        ca_container(rt, object && !pairs ? CA_DICT_T : CA_LIST_T, 0, NULL);
    whitespace(p);
    if (p->at < p->length && p->text[p->at] == end)
      p->at++;
    else
      for (;;) {
        if (object) {
          if (p->at >= p->length || p->text[p->at] != '"') {
            error(p, "Expecting property name enclosed in double quotes");
            break;
          }
          roots[1] = string(p);
          if (rt->error)
            break;
          whitespace(p);
          if (p->at >= p->length || p->text[p->at++] != ':') {
            error(p, "Expecting ':' delimiter");
            break;
          }
          roots[2] = parse(p);
          if (rt->error)
            break;
          if (pairs) {
            CaValue values[] = {roots[1], roots[2]};
            CaValue pair = ca_container(rt, CA_TUPLE_T, 2, values);
            if (!rt->error)
              ca_append(rt, roots[0], pair);
          } else
            ca_setitem(rt, roots[0], roots[1], roots[2]);
        } else {
          roots[2] = parse(p);
          if (!rt->error)
            ca_append(rt, roots[0], roots[2]);
        }
        if (rt->error)
          break;
        whitespace(p);
        if (p->at < p->length && p->text[p->at] == end) {
          p->at++;
          break;
        }
        if (p->at >= p->length || p->text[p->at++] != ',') {
          error(p, "Expecting ',' delimiter");
          break;
        }
        whitespace(p);
      }
    if (object && !rt->error) {
      CaValue hook = pairs ? p->options[5] : p->options[1];
      if (hook.tag != CA_NONE_T)
        roots[0] = ca_call(rt, hook, 1, 0, roots, NULL);
    }
    goto done;
  }
  struct {
    const char *text;
    int length;
    int kind;
  } literals[] = {{"null", 4, 0}, {"true", 4, 1},     {"false", 5, 2},
                  {"NaN", 3, 3},  {"Infinity", 8, 4}, {"-Infinity", 9, 5}};
  for (size_t i = 0; i < sizeof literals / sizeof literals[0]; i++)
    if (p->length - p->at >= (size_t)literals[i].length &&
        !memcmp(p->text + p->at, literals[i].text,
                (size_t)literals[i].length)) {
      p->at += (size_t)literals[i].length;
      if (i < 3)
        roots[0] = i ? ca_bool(i == 1) : CA_NONE;
      else if (p->options[4].tag != CA_NONE_T) {
        CaValue token =
            ca_string(rt, literals[i].text, (size_t)literals[i].length);
        if (!rt->error)
          roots[0] = ca_call(rt, p->options[4], 1, 0, &token, NULL);
      } else
        roots[0] = ca_float(i == 3 ? NAN : i == 4 ? INFINITY : -INFINITY);
      goto done;
    }
  if (c == '-' || (c >= '0' && c <= '9')) {
    size_t start = p->at;
    if (p->text[p->at] == '-')
      p->at++;
    if (p->at >= p->length) {
      error(p, "Expecting value");
      goto done;
    }
    if (p->text[p->at] == '0')
      p->at++;
    else if (p->text[p->at] >= '1' && p->text[p->at] <= '9') {
      while (p->at < p->length && isdigit((unsigned char)p->text[p->at]))
        p->at++;
    } else {
      error(p, "Expecting value");
      goto done;
    }
    int floating = 0;
    if (p->at < p->length && p->text[p->at] == '.') {
      floating = 1;
      p->at++;
      if (p->at >= p->length || !isdigit((unsigned char)p->text[p->at])) {
        error(p, "Expecting fractional digit");
        goto done;
      }
      while (p->at < p->length && isdigit((unsigned char)p->text[p->at]))
        p->at++;
    }
    if (p->at < p->length && (p->text[p->at] == 'e' || p->text[p->at] == 'E')) {
      floating = 1;
      p->at++;
      if (p->at < p->length && (p->text[p->at] == '+' || p->text[p->at] == '-'))
        p->at++;
      if (p->at >= p->length || !isdigit((unsigned char)p->text[p->at])) {
        error(p, "Expecting exponent digit");
        goto done;
      }
      while (p->at < p->length && isdigit((unsigned char)p->text[p->at]))
        p->at++;
    }
    CaValue hook = p->options[floating ? 2 : 3];
    roots[1] = ca_string(rt, p->text + start, p->at - start);
    if (rt->error)
      goto done;
    if (hook.tag != CA_NONE_T)
      roots[0] = ca_call(rt, hook, 1, 0, &roots[1], NULL);
    else if (floating)
      roots[0] = ca_float(strtod(((CaString *)roots[1].as.obj)->data, NULL));
    else if (p->at - start > 4300 + (c == '-'))
      ca_error(rt, "ValueError",
               "integer conversion exceeds native 4300-digit limit");
    else
      roots[0] = ca_integer(rt, ((CaString *)roots[1].as.obj)->data);
    goto done;
  }
  error(p, "Expecting value");
done:;
  CaValue result = roots[0];
  ca_pop(rt, &frame);
  ca_leave(rt);
  return rt->error ? CA_UNBOUND : result;
}
typedef struct {
  CaRuntime *rt;
  FILE *stream;
  CaValue *options;
  CaValue item_separator, key_separator, indent;
  int pretty, ensure_ascii, skipkeys, circular, allow_nan, sort;
  uintptr_t seen[256];
  CaTag tags[256];
  size_t depth;
} Encoder;
static void literal(Encoder *e, const char *text) { fputs(text, e->stream); }
static void quoted(Encoder *e, CaString *s) {
  fputc('"', e->stream);
  for (size_t i = 0; i < s->length;) {
    size_t start = i;
    uint32_t c = (unsigned char)s->data[i++];
    if (c >= 0xc0) {
      int n = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
      c &= n == 1 ? 31 : n == 2 ? 15 : 7;
      while (n-- && i < s->length)
        c = (c << 6) | ((unsigned char)s->data[i++] & 63);
    }
    switch (c) {
    case '"':
      literal(e, "\\\"");
      continue;
    case '\\':
      literal(e, "\\\\");
      continue;
    case '\b':
      literal(e, "\\b");
      continue;
    case '\f':
      literal(e, "\\f");
      continue;
    case '\n':
      literal(e, "\\n");
      continue;
    case '\r':
      literal(e, "\\r");
      continue;
    case '\t':
      literal(e, "\\t");
      continue;
    default:
      break;
    }
    if (c < 32 || (e->ensure_ascii && c >= 127)) {
      if (c <= 65535)
        fprintf(e->stream, "\\u%04x", c);
      else {
        c -= 0x10000;
        fprintf(e->stream, "\\u%04x\\u%04x", 0xd800 + (c >> 10),
                0xdc00 + (c & 1023));
      }
    } else
      fwrite(s->data + start, 1, i - start, e->stream);
  }
  fputc('"', e->stream);
}
static void write_text(Encoder *e, CaValue value) {
  CaString *s = (CaString *)value.as.obj;
  fwrite(s->data, 1, s->length, e->stream);
}
static void newline(Encoder *e, size_t depth) {
  if (!e->pretty)
    return;
  fputc('\n', e->stream);
  for (size_t i = 0; i < depth; i++)
    write_text(e, e->indent);
}
static CaValue key_text(Encoder *e, CaValue key) {
  if (key.tag == CA_STR_T)
    return key;
  if (key.tag == CA_NONE_T)
    return ca_string(e->rt, "null", 4);
  if (key.tag == CA_BOOL_T)
    return ca_string(e->rt, key.as.i ? "true" : "false", key.as.i ? 4 : 5);
  if (key.tag == CA_INT_T || key.tag == CA_BIG_T)
    return ca_stringify(e->rt, key, 0);
  if (key.tag == CA_FLOAT_T) {
    if (!isfinite(key.as.f)) {
      if (!e->allow_nan) {
        ca_error(e->rt, "ValueError",
                 "Out of range float values are not JSON compliant");
        return CA_UNBOUND;
      }
      const char *name = isnan(key.as.f)     ? "NaN"
                         : signbit(key.as.f) ? "-Infinity"
                                             : "Infinity";
      return ca_string(e->rt, name, strlen(name));
    }
    return ca_stringify(e->rt, key, 0);
  }
  if (!e->skipkeys)
    ca_error(e->rt, "TypeError", "keys must be str, int, float, bool or None");
  return CA_UNBOUND;
}
static void encode(Encoder *e, CaValue value) {
  CaRuntime *rt = e->rt;
  if (!ca_enter(rt))
    return;
  CaValue roots[] = {value, CA_UNBOUND, CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 3);
  ca_safepoint(rt);
  if (value.tag == CA_STR_T) {
    quoted(e, (CaString *)value.as.obj);
    goto done;
  }
  if (value.tag == CA_NONE_T) {
    literal(e, "null");
    goto done;
  }
  if (value.tag == CA_BOOL_T) {
    literal(e, value.as.i ? "true" : "false");
    goto done;
  }
  if (value.tag == CA_INT_T || value.tag == CA_BIG_T) {
    ca_write_integer(rt, value, e->stream);
    goto done;
  }
  if (value.tag == CA_FLOAT_T) {
    if (!isfinite(value.as.f)) {
      if (!e->allow_nan)
        ca_error(rt, "ValueError",
                 "Out of range float values are not JSON compliant");
      else
        literal(e, isnan(value.as.f)     ? "NaN"
                   : signbit(value.as.f) ? "-Infinity"
                                         : "Infinity");
    } else
      ca_write_float(value.as.f, e->stream);
    goto done;
  }
  int collection = value.tag == CA_LIST_T || value.tag == CA_TUPLE_T ||
                   value.tag == CA_DICT_T;
  if (!collection && e->options[8].tag == CA_NONE_T) {
    ca_error(rt, "TypeError", "Object is not JSON serializable");
    goto done;
  }
  uintptr_t identity = value.tag == CA_FUNCTION_T ? (uintptr_t)value.as.fn
                                                  : (uintptr_t)value.as.obj;
  if (e->circular)
    for (size_t i = 0; i < e->depth; i++)
      if (e->tags[i] == value.tag && e->seen[i] == identity) {
        ca_error(rt, "ValueError", "Circular reference detected");
        goto done;
      }
  if (e->depth >= 256) {
    ca_error(rt, "RecursionError", "native JSON recursion limit exceeded");
    goto done;
  }
  e->seen[e->depth] = identity;
  e->tags[e->depth] = value.tag;
  e->depth++;
  if (!collection) {
    roots[1] = ca_call(rt, e->options[8], 1, 0, &roots[0], NULL);
    if (!rt->error)
      encode(e, roots[1]);
    e->depth--;
    goto done;
  }
  CaContainer *c = (CaContainer *)value.as.obj;
  if (value.tag == CA_DICT_T) {
    fputc('{', e->stream);
    size_t printed = 0;
    if (e->sort) {
      roots[1] = ca_container(rt, CA_LIST_T, 0, NULL);
      for (size_t i = 0; i < c->length && !rt->error; i += 2)
        ca_append(rt, roots[1], c->items[i]);
      if (!rt->error)
        roots[1] = ca_extended_call(rt, 25, 1, 0, &roots[1], NULL);
    }
    size_t entries = c->length / 2;
    for (size_t i = 0; i < entries && !rt->error; i++) {
      if (c->length / 2 != entries) {
        ca_error(rt, "RuntimeError",
                 "dictionary changed size during JSON encoding");
        break;
      }
      CaValue key = e->sort ? ((CaContainer *)roots[1].as.obj)->items[i]
                            : c->items[i * 2];
      roots[2] = key_text(e, key);
      if (rt->error)
        break;
      if (roots[2].tag == CA_UNBOUND_T)
        continue;
      if (printed++)
        write_text(e, e->item_separator);
      newline(e, e->depth);
      quoted(e, (CaString *)roots[2].as.obj);
      write_text(e, e->key_separator);
      roots[2] = e->sort ? ca_getitem(rt, value, key) : c->items[i * 2 + 1];
      if (!rt->error)
        encode(e, roots[2]);
    }
    if (printed)
      newline(e, e->depth - 1);
    fputc('}', e->stream);
  } else {
    fputc('[', e->stream);
    for (size_t i = 0; i < c->length && !rt->error; i++) {
      if (i)
        write_text(e, e->item_separator);
      newline(e, e->depth);
      encode(e, c->items[i]);
    }
    if (c->length)
      newline(e, e->depth - 1);
    fputc(']', e->stream);
  }
  e->depth--;
done:
  ca_pop(rt, &frame);
  ca_leave(rt);
}
CaValue ca_json_call(CaRuntime *rt, int kind, int n, int nkw, CaValue *args,
                     const char **keys) {
  if (n != 1) {
    ca_error(rt, "TypeError", "JSON APIs require one positional argument");
    return CA_UNBOUND;
  }
  if (kind == N_JSON_LOADS) {
    const char *names[] = {"s",
                           "object_hook",
                           "parse_float",
                           "parse_int",
                           "parse_constant",
                           "object_pairs_hook",
                           "strict",
                           "cls"};
    CaValue defaults[] = {CA_NONE, CA_NONE, CA_NONE,    CA_NONE,
                          CA_NONE, CA_NONE, ca_bool(1), CA_NONE},
            out[8];
    if (!ca_native_arguments(rt, n, nkw, args, keys, 8, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    if (out[7].tag != CA_NONE_T) {
      ca_error(rt, "NotImplementedError",
               "custom JSON decoder classes are not implemented");
      return CA_UNBOUND;
    }
    CaFrame frame;
    ca_push(rt, &frame, out, 8);
    if (out[0].tag == CA_BYTES_T) {
      CaString *s = (CaString *)out[0].as.obj;
      if (s->length >= 2 && (((unsigned char)s->data[0] == 0xff &&
                              (unsigned char)s->data[1] == 0xfe) ||
                             ((unsigned char)s->data[0] == 0xfe &&
                              (unsigned char)s->data[1] == 0xff))) {
        ca_error(rt, "NotImplementedError",
                 "UTF-16/32 JSON byte input is not implemented");
        ca_pop(rt, &frame);
        return CA_UNBOUND;
      }
      CaValue values[] = {out[0], ca_string(rt, "utf-8-sig", 9)};
      out[0] = ca_text_call(rt, N_TEXT_DECODE, 2, 0, values, NULL);
    }
    if (!rt->error && out[0].tag != CA_STR_T)
      ca_error(rt, "TypeError", "JSON input must be str or bytes");
    CaValue result = CA_UNBOUND;
    if (!rt->error) {
      CaString *s = (CaString *)out[0].as.obj;
      Parser parser = {.rt = rt,
                       .input = out[0],
                       .options = out,
                       .text = s->data,
                       .length = s->length};
      if (s->length >= 3 && !memcmp(s->data, "\xef\xbb\xbf", 3))
        error(&parser, "Unexpected UTF-8 BOM");
      else
        result = parse(&parser);
      if (!rt->error) {
        whitespace(&parser);
        if (parser.at != parser.length)
          error(&parser, "Extra data");
      }
    }
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : result;
  }
  const char *names[] = {
      "obj", "skipkeys", "ensure_ascii", "check_circular", "allow_nan",
      "cls", "indent",   "separators",   "default",        "sort_keys"};
  CaValue defaults[] = {CA_NONE,    ca_bool(0), ca_bool(1), ca_bool(1),
                        ca_bool(1), CA_NONE,    CA_NONE,    CA_NONE,
                        CA_NONE,    ca_bool(0)},
          out[10];
  if (!ca_native_arguments(rt, n, nkw, args, keys, 10, 1, names, defaults, out))
    return CA_UNBOUND;
  if (out[5].tag != CA_NONE_T) {
    ca_error(rt, "NotImplementedError",
             "custom JSON encoder classes are not implemented");
    return CA_UNBOUND;
  }
  CaValue rooted[13];
  for (size_t i = 0; i < 10; i++)
    rooted[i] = out[i];
  for (size_t i = 10; i < 13; i++)
    rooted[i] = CA_UNBOUND;
  CaFrame frame;
  ca_push(rt, &frame, rooted, 13);
  Encoder encoder = {.rt = rt,
                     .options = rooted,
                     .pretty = out[6].tag != CA_NONE_T,
                     .ensure_ascii = ca_truth(rt, out[2]),
                     .skipkeys = ca_truth(rt, out[1]),
                     .circular = ca_truth(rt, out[3]),
                     .allow_nan = ca_truth(rt, out[4]),
                     .sort = ca_truth(rt, out[9])};
  if (encoder.pretty) {
    if (out[6].tag == CA_STR_T)
      rooted[10] = out[6];
    else if (out[6].tag == CA_INT_T || out[6].tag == CA_BOOL_T) {
      if (out[6].as.i > 1000000)
        ca_error(rt, "OverflowError", "native indentation limit exceeded");
      else {
        size_t count = out[6].as.i > 0 ? (size_t)out[6].as.i : 0;
        char *spaces = malloc(count ? count : 1);
        if (!spaces)
          ca_error(rt, "MemoryError", "JSON indentation allocation failed");
        else {
          memset(spaces, ' ', count);
          rooted[10] = ca_string(rt, spaces, count);
          free(spaces);
        }
      }
    } else
      ca_error(rt, "TypeError", "indent must be int or str");
  } else
    rooted[10] = ca_string(rt, "", 0);
  rooted[11] =
      ca_string(rt, encoder.pretty ? "," : ", ", encoder.pretty ? 1 : 2);
  rooted[12] = ca_string(rt, ": ", 2);
  if (out[7].tag != CA_NONE_T) {
    if ((out[7].tag != CA_TUPLE_T && out[7].tag != CA_LIST_T) ||
        ((CaContainer *)out[7].as.obj)->length != 2)
      ca_error(rt, "ValueError", "separators must contain two strings");
    else {
      CaContainer *separators = (CaContainer *)out[7].as.obj;
      rooted[11] = separators->items[0];
      rooted[12] = separators->items[1];
      if (rooted[11].tag != CA_STR_T || rooted[12].tag != CA_STR_T)
        ca_error(rt, "TypeError", "separators must be strings");
    }
  }
  encoder.indent = rooted[10];
  encoder.item_separator = rooted[11];
  encoder.key_separator = rooted[12];
  char *buffer = NULL;
  size_t length = 0;
  CaValue result = CA_UNBOUND;
  if (!rt->error) {
    encoder.stream = open_memstream(&buffer, &length);
    if (!encoder.stream)
      ca_error(rt, "MemoryError", "JSON output allocation failed");
    else {
      encode(&encoder, out[0]);
      if (fclose(encoder.stream) && !rt->error)
        ca_error(rt, "MemoryError", "JSON output allocation failed");
      if (!rt->error)
        result = ca_string(rt, buffer, length);
      free(buffer);
    }
  }
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : result;
}
