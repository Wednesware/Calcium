#define _GNU_SOURCE
#include "standard.h"
#include <inttypes.h>
static void quote(FILE *out, CaString *s, int ascii, int bytes) {
  int single = 0, dbl = 0;
  for (size_t i = 0; i < s->length; i++) {
    single |= s->data[i] == '\'';
    dbl |= s->data[i] == '"';
  }
  char q = single && !dbl ? '"' : '\'';
  if (bytes)
    fputc('b', out);
  fputc(q, out);
  for (size_t i = 0; i < s->length;) {
    size_t start = i;
    uint32_t c = (unsigned char)s->data[i++];
    if (!bytes && c >= 0xc0) {
      int more = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
      c &= more == 1 ? 31 : more == 2 ? 15 : 7;
      while (more-- && i < s->length)
        c = (c << 6) | ((unsigned char)s->data[i++] & 63);
    }
    if (c == (unsigned char)q || c == '\\') {
      fputc('\\', out);
      fputc((int)c, out);
    } else if (c == '\n')
      fputs("\\n", out);
    else if (c == '\r')
      fputs("\\r", out);
    else if (c == '\t')
      fputs("\\t", out);
    else if (c < 32 || c == 127 || (bytes && c >= 128) || (ascii && c >= 128) ||
             (!bytes && !ca_unicode_printable(c))) {
      if (c <= 255)
        fprintf(out, "\\x%02x", c);
      else if (c <= 65535)
        fprintf(out, "\\u%04x", c);
      else
        fprintf(out, "\\U%08x", c);
    } else
      fwrite(s->data + start, 1, i - start, out);
  }
  fputc(q, out);
}
static void render(CaRuntime *, FILE *, CaValue, int);
static void instance(CaRuntime *rt, FILE *out, CaValue value, int mode) {
  CaInstance *i = (CaInstance *)value.as.obj;
  CaValue method = ca_try_special(rt, value, mode ? "__repr__" : "__str__");
  if (!mode && method.tag == CA_UNBOUND_T)
    method = ca_try_special(rt, value, "__repr__");
  if (method.tag != CA_UNBOUND_T) {
    CaValue text = ca_call(rt, method, 0, 0, NULL, NULL);
    if (rt->error)
      return;
    if (text.tag != CA_STR_T) {
      ca_error(rt, "TypeError", "__str__/__repr__ must return str");
      return;
    }
    CaString *s = (CaString *)text.as.obj;
    fwrite(s->data, 1, s->length, out);
    return;
  }
  CaClass *type = (CaClass *)i->type.as.obj;
  if ((type->dataclass & 4) && type->fields.tag == CA_LIST_T) {
    if (i->printing) {
      fputs("...", out);
      return;
    }
    i->printing = 1;
    fprintf(out, "%s(", ca_type_name(i->type));
    CaContainer *fields = (CaContainer *)type->fields.as.obj;
    size_t printed = 0;
    for (size_t k = 0; k < fields->length && !rt->error; k++) {
      CaString *name = (CaString *)fields->items[k].as.obj;
      if (!ca_dataclass_repr_field(i->type, name->data))
        continue;
      if (printed++)
        fputs(", ", out);
      fwrite(name->data, 1, name->length, out);
      fputc('=', out);
      render(rt, out, ca_attribute(rt, value, name->data, 0), 1);
    }
    fputc(')', out);
    i->printing = 0;
    return;
  }
  CaValue module = ca_attribute(rt, i->type, "__module__", 0);
  if (rt->error)
    ca_exception_take(rt);
  fprintf(out, "<%s%s%s object at %p>",
          module.tag == CA_STR_T ? ((CaString *)module.as.obj)->data : "",
          module.tag == CA_STR_T ? "." : "", ca_type_name(i->type), (void *)i);
}
static void render(CaRuntime *rt, FILE *out, CaValue v, int mode) {
  if (rt->error)
    return;
  if (!ca_enter(rt))
    return;
  switch (v.tag) {
  case CA_NONE_T:
    fputs("None", out);
    break;
  case CA_BOOL_T:
    fputs(v.as.i ? "True" : "False", out);
    break;
  case CA_INT_T:
  case CA_BIG_T:
    ca_write_integer(rt, v, out);
    break;
  case CA_FLOAT_T:
    ca_write_float(v.as.f, out);
    break;
  case CA_STR_T: {
    CaString *s = (CaString *)v.as.obj;
    if (mode)
      quote(out, s, mode == 2, 0);
    else
      fwrite(s->data, 1, s->length, out);
    break;
  }
  case CA_BYTES_T:
    quote(out, (CaString *)v.as.obj, 1, 1);
    break;
  case CA_LIST_T:
  case CA_TUPLE_T:
  case CA_DICT_T:
  case CA_SET_T: {
    CaContainer *c = (CaContainer *)v.as.obj;
    if (c->printing) {
      fputs(v.tag == CA_LIST_T   ? "[...]"
            : v.tag == CA_DICT_T ? "{...}"
                                 : "(...)",
            out);
      break;
    }
    if (v.tag == CA_SET_T && !c->length) {
      fputs("set()", out);
      break;
    }
    c->printing = 1;
    fputc(v.tag == CA_LIST_T ? '[' : v.tag == CA_TUPLE_T ? '(' : '{', out);
    for (size_t i = 0; i < c->length && !rt->error; i++) {
      if (i)
        fputs(", ", out);
      render(rt, out, c->items[i], mode == 2 ? 2 : 1);
      if (v.tag == CA_DICT_T) {
        fputs(": ", out);
        render(rt, out, c->items[++i], mode == 2 ? 2 : 1);
      }
    }
    if (v.tag == CA_TUPLE_T && c->length == 1)
      fputc(',', out);
    fputc(v.tag == CA_LIST_T ? ']' : v.tag == CA_TUPLE_T ? ')' : '}', out);
    c->printing = 0;
    break;
  }
  case CA_CLASS_T:
  case CA_TYPE_T:
    fprintf(out, "<class '%s'>", ca_type_name(v));
    break;
  case CA_FUNCTION_T:
  case CA_CLOSURE_T: {
    const CaFunction *f =
        v.tag == CA_CLOSURE_T ? &((CaClosure *)v.as.obj)->fn : v.as.fn;
    if (f->builtin == 2 || (f->builtin >= 4 && f->builtin <= 7))
      fprintf(out, "<class '%s'>", f->name);
    else if (f->builtin)
      fprintf(out, "<built-in function %s>", f->name);
    else
      fprintf(out, "<function %s at %p>", f->name, (const void *)f);
    break;
  }
  case CA_INSTANCE_T:
    instance(rt, out, v, mode);
    break;
  case CA_EXCEPTION_T: {
    CaException *e = (CaException *)v.as.obj;
    if (mode) {
      const char *name = ca_type_name(e->type), *dot = strrchr(name, '.');
      fprintf(out, "%s(", dot ? dot + 1 : name);
    }
    if (e->args.tag == CA_UNBOUND_T || (!mode && e->message.tag == CA_STR_T)) {
      render(rt, out, e->message, mode ? 1 : 0);
    } else {
      CaContainer *args = (CaContainer *)e->args.as.obj;
      if (mode) {
        for (size_t i = 0; i < args->length; i++) {
          if (i)
            fputs(", ", out);
          render(rt, out, args->items[i], 1);
        }
      } else if (args->length == 1)
        render(rt, out, args->items[0],
               ca_subclass(rt, e->type, ca_type(CA_T_KEYERROR)) ? 1 : 0);
      else if (args->length)
        render(rt, out, e->args, 1);
    }
    if (mode)
      fputc(')', out);
    break;
  }
  case CA_RANGE_T: {
    CaRange *r = (CaRange *)v.as.obj;
    fputs("range(", out);
    render(rt, out, r->start, 0);
    fputs(", ", out);
    render(rt, out, r->stop, 0);
    if (ca_integer_compare(r->step, ca_int(1))) {
      fputs(", ", out);
      render(rt, out, r->step, 0);
    }
    fputc(')', out);
    break;
  }
  case CA_FOREIGN_T:
    ca_foreign_render(rt, out, v, mode);
    break;
  default:
    ca_error(rt, "TypeError", "native value has no string representation");
  }
  ca_leave(rt);
}
CaValue ca_stringify(CaRuntime *rt, CaValue value, int mode) {
  if (value.tag == CA_STR_T && !mode)
    return value;
  char *text = NULL;
  size_t length = 0;
  FILE *stream = open_memstream(&text, &length);
  if (!stream) {
    ca_error(rt, "MemoryError", "string formatting allocation failed");
    return CA_UNBOUND;
  }
  CaFrame frame;
  ca_push(rt, &frame, &value, 1);
  render(rt, stream, value, mode);
  ca_pop(rt, &frame);
  if (fclose(stream) && !rt->error)
    ca_error(rt, "MemoryError", "string formatting allocation failed");
  CaValue result = rt->error ? CA_UNBOUND : ca_string(rt, text, length);
  free(text);
  return result;
}
CaValue ca_join_strings(CaRuntime *rt, size_t n, CaValue *values) {
  size_t length = 0;
  for (size_t i = 0; i < n; i++) {
    if (values[i].tag != CA_STR_T) {
      ca_error(rt, "TypeError", "string join expects strings");
      return CA_UNBOUND;
    }
    size_t count = ((CaString *)values[i].as.obj)->length;
    if (count > SIZE_MAX - length) {
      ca_error(rt, "MemoryError", "joined string too large");
      return CA_UNBOUND;
    }
    length += count;
  }
  if (length > SIZE_MAX - sizeof(CaString) - 1) {
    ca_error(rt, "MemoryError", "joined string too large");
    return CA_UNBOUND;
  }
  CaString *out = ca_allocate(rt, sizeof *out + length + 1, CA_STR_T);
  if (!out)
    return CA_UNBOUND;
  out->length = length;
  size_t at = 0;
  for (size_t i = 0; i < n; i++) {
    CaString *s = (CaString *)values[i].as.obj;
    memcpy(out->data + at, s->data, s->length);
    at += s->length;
  }
  return (CaValue){.tag = CA_STR_T, .as.obj = (CaObj *)out};
}
CaValue ca_format(CaRuntime *rt, CaValue value, CaValue specification) {
  if (specification.tag != CA_STR_T) {
    ca_error(rt, "TypeError", "format specifier must be str");
    return CA_UNBOUND;
  }
  CaString *spec = (CaString *)specification.as.obj;
  if (value.tag == CA_INSTANCE_T) {
    CaValue method = ca_try_special(rt, value, "__format__");
    if (method.tag != CA_UNBOUND_T)
      return ca_call(rt, method, 1, 0, &specification, NULL);
  }
  if (!spec->length)
    return ca_stringify(rt, value, 0);
  /* Bounded, explicitly parsed common width/precision forms. Never pass a
     Python-controlled specifier through to printf. */
  const char *s = spec->data;
  size_t at = 0;
  char align = 0, fill = ' ';
  if (spec->length > 1 && (s[1] == '<' || s[1] == '>' || s[1] == '^')) {
    fill = s[0];
    align = s[1];
    at = 2;
  } else if (s[0] == '<' || s[0] == '>' || s[0] == '^') {
    align = s[0];
    at = 1;
  }
  int zero = 0;
  if (s[at] == '0') {
    zero = 1;
    at++;
  }
  size_t width = 0;
  while (s[at] >= '0' && s[at] <= '9') {
    if (width > 100000) {
      ca_error(rt, "ValueError", "format width exceeds native limit");
      return CA_UNBOUND;
    }
    width = width * 10 + (unsigned)(s[at++] - '0');
  }
  int precision = -1;
  if (s[at] == '.') {
    at++;
    precision = 0;
    if (s[at] < '0' || s[at] > '9') {
      ca_error(rt, "ValueError", "missing format precision");
      return CA_UNBOUND;
    }
    while (s[at] >= '0' && s[at] <= '9') {
      if (precision > 10000) {
        ca_error(rt, "ValueError", "format precision exceeds native limit");
        return CA_UNBOUND;
      }
      precision = precision * 10 + s[at++] - '0';
    }
  }
  char kind = s[at];
  if (kind)
    at++;
  if (at != spec->length || (kind && kind != 's' && kind != 'd' &&
                             kind != 'f' && kind != 'e' && kind != 'g')) {
    ca_error(rt, "NotImplementedError",
             "native format specifier is not implemented");
    return CA_UNBOUND;
  }
  CaValue text = CA_UNBOUND;
  if (kind == 'f' || kind == 'e' || kind == 'g') {
    double number = ca_as_double(rt, value);
    if (rt->error)
      return CA_UNBOUND;
    int p = precision < 0 ? 6 : precision;
    const char *format = kind == 'f' ? "%.*f" : kind == 'e' ? "%.*e" : "%.*g";
    int length = snprintf(NULL, 0, format, p, number);
    if (length < 0) {
      ca_error(rt, "ValueError", "numeric format failed");
      return CA_UNBOUND;
    }
    char *buffer = malloc((size_t)length + 1);
    if (!buffer) {
      ca_error(rt, "MemoryError", "format allocation failed");
      return CA_UNBOUND;
    }
    snprintf(buffer, (size_t)length + 1, format, p, number);
    text = ca_string(rt, buffer, (size_t)length);
    free(buffer);
  } else {
    if ((kind == 's' && value.tag != CA_STR_T) ||
        (kind == 'd' && value.tag != CA_INT_T && value.tag != CA_BIG_T &&
         value.tag != CA_BOOL_T)) {
      ca_error(rt, "ValueError", "format code does not match native type");
      return CA_UNBOUND;
    }
    if (precision >= 0 && value.tag != CA_STR_T) {
      ca_error(rt, "ValueError", "precision not allowed for integer format");
      return CA_UNBOUND;
    }
    text = ca_stringify(
        rt, value.tag == CA_BOOL_T && kind == 'd' ? ca_int(value.as.i) : value,
        0);
    if (precision >= 0 && !rt->error) {
      CaValue slice = ca_slice(rt, CA_NONE, ca_int(precision), CA_NONE);
      text = ca_getitem(rt, text, slice);
    }
  }
  if (rt->error)
    return CA_UNBOUND;
  CaString *original = (CaString *)text.as.obj;
  size_t length = 0;
  for (size_t i = 0; i < original->length; i++)
    if (((unsigned char)original->data[i] & 0xc0) != 0x80)
      length++;
  if (width <= length)
    return text;
  if (!align)
    align = value.tag == CA_STR_T ? '<' : '>';
  size_t padding = width - length, left = align == '<'   ? 0
                                          : align == '^' ? padding / 2
                                                         : padding;
  size_t total = original->length + padding;
  char *buffer = malloc(total + 1);
  if (!buffer) {
    ca_error(rt, "MemoryError", "format allocation failed");
    return CA_UNBOUND;
  }
  memset(buffer, zero ? '0' : fill, total);
  memcpy(buffer + left, original->data, original->length);
  if (zero && left && original->length &&
      (original->data[0] == '-' || original->data[0] == '+')) {
    buffer[0] = original->data[0];
    buffer[left] = '0';
  }
  CaValue result = ca_string(rt, buffer, total);
  free(buffer);
  return result;
}
