#define _GNU_SOURCE
#include "standard.h"
#include <ctype.h>
static size_t advance(const char *text, size_t length, size_t index,
                      int bytes) {
  index++;
  if (!bytes)
    while (index < length && ((unsigned char)text[index] & 0xc0) == 0x80)
      index++;
  return index;
}
static size_t retreat(const char *text, size_t index, int bytes) {
  if (index)
    index--;
  if (!bytes)
    while (index && ((unsigned char)text[index] & 0xc0) == 0x80)
      index--;
  return index;
}
static uint32_t point(const char *s, size_t start, size_t stop) {
  uint32_t cp = (unsigned char)s[start++];
  if (cp >= 0xc0 && stop > start) {
    cp &= stop - start == 1 ? 31 : stop - start == 2 ? 15 : 7;
    while (start < stop)
      cp = (cp << 6) | ((unsigned char)s[start++] & 63);
  }
  return cp;
}
static int index_arg(CaRuntime *rt, CaValue v, int64_t *out) {
  if (v.tag == CA_INT_T || v.tag == CA_BOOL_T) {
    *out = v.as.i;
    return 1;
  }
  if (v.tag == CA_BIG_T) {
    *out = ((CaBig *)v.as.obj)->sign < 0 ? INT64_MIN : INT64_MAX;
    return 1;
  }
  ca_error(rt, "TypeError", "an integer is required");
  return 0;
}
static int codec(CaRuntime *rt, CaValue value) {
  if (value.tag == CA_NONE_T)
    return 1;
  const char *name = ca_native_text(rt, value, 0);
  if (!name)
    return 0;
  char normal[48];
  size_t n = strlen(name);
  if (n >= sizeof normal) {
    ca_error(rt, "LookupError", "unknown encoding");
    return 0;
  }
  for (size_t i = 0; i <= n; i++)
    normal[i] = name[i] == '_' ? '-' : (char)tolower((unsigned char)name[i]);
  if (!strcmp(normal, "utf-8") || !strcmp(normal, "utf8"))
    return 1;
  if (!strcmp(normal, "utf-8-sig"))
    return 2;
  if (!strcmp(normal, "ascii") || !strcmp(normal, "us-ascii"))
    return 3;
  ca_error(rt, "LookupError", "native encoding is not implemented");
  return 0;
}
static int errors(CaRuntime *rt, CaValue value) {
  if (value.tag == CA_NONE_T)
    return 0;
  const char *name = ca_native_text(rt, value, 0);
  if (!name)
    return -1;
  if (!strcmp(name, "strict"))
    return 0;
  if (!strcmp(name, "ignore"))
    return 1;
  if (!strcmp(name, "replace"))
    return 2;
  ca_error(rt, "NotImplementedError",
           "native codec error handler is not implemented");
  return -1;
}
static int utf8(const unsigned char *s, size_t n, size_t *span) {
  unsigned c = s[0];
  *span = 1;
  if (c < 128)
    return 1;
  size_t count = c >= 0xc2 && c <= 0xdf   ? 2
                 : c >= 0xe0 && c <= 0xef ? 3
                 : c >= 0xf0 && c <= 0xf4 ? 4
                                          : 0;
  if (!count)
    return 0;
  for (size_t i = 1; i < count; i++) {
    if (i >= n) {
      *span = i;
      return 0;
    }
    if ((s[i] & 0xc0) != 0x80) {
      *span = i;
      return 0;
    }
    if (i == 1 && ((c == 0xe0 && s[i] < 0xa0) || (c == 0xed && s[i] >= 0xa0) ||
                   (c == 0xf0 && s[i] < 0x90) || (c == 0xf4 && s[i] >= 0x90)))
      return 0;
  }
  *span = count;
  return 1;
}
static CaValue from_stream(CaRuntime *rt, FILE *stream, char **buffer,
                           size_t *length, int bytes) {
  if (fclose(stream) && !rt->error)
    ca_error(rt, "MemoryError", "text buffer allocation failed");
  CaValue out = rt->error ? CA_UNBOUND
                : bytes   ? ca_bytes(rt, *buffer, *length)
                          : ca_string(rt, *buffer, *length);
  free(*buffer);
  return out;
}
static CaValue fragment(CaRuntime *rt, CaValue value, size_t start,
                        size_t end) {
  CaString *s = (CaString *)value.as.obj;
  return value.tag == CA_BYTES_T ? ca_bytes(rt, s->data + start, end - start)
                                 : ca_string(rt, s->data + start, end - start);
}
static int space(CaString *s, size_t a, size_t b, int bytes) {
  uint32_t c = point(s->data, a, b);
  return bytes ? (c == ' ' || (c >= 9 && c <= 13)) : ca_unicode_space(c);
}
CaValue ca_text_call(CaRuntime *rt, int kind, int n, int nkw, CaValue *args,
                     const char **keys) {
  if (kind == N_DICT_FROMKEYS) {
    CaValue out[2], defaults[] = {CA_NONE, CA_NONE};
    const char *names[] = {"iterable", "value"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 2, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    CaValue roots[] = {ca_container(rt, CA_DICT_T, 0, NULL),
                       ca_iter(rt, out[0]), CA_UNBOUND, out[1]};
    CaFrame frame;
    ca_push(rt, &frame, roots, 4);
    if (!rt->error)
      while (ca_next(rt, roots[1], &roots[2]) > 0 && !rt->error) {
        ca_setitem(rt, roots[0], roots[2], roots[3]);
        if (rt->error)
          break;
        ca_safepoint(rt);
      }
    CaValue result = roots[0];
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : result;
  }
  if (kind >= N_DICT_ITEMS && kind <= N_DICT_VALUES) {
    if (n != 1 || nkw || args[0].tag != CA_DICT_T) {
      ca_error(rt, "TypeError", "invalid dictionary view arguments");
      return CA_UNBOUND;
    }
    return ca_native_object(rt, CA_F_DICT_VIEW, args[0], NULL,
                            kind == N_DICT_ITEMS    ? 2
                            : kind == N_DICT_VALUES ? 1
                                                    : 0);
  }
  CaValue out[4], defaults[4] = {CA_NONE, CA_NONE, CA_NONE, CA_NONE};
  const char *names[4] = {"self", "value", "start", "end"};
  int count = 1, required = 1;
  switch (kind) {
  case N_TEXT_ENCODE:
  case N_TEXT_DECODE:
    count = 3;
    names[1] = "encoding";
    names[2] = "errors";
    break;
  case N_TEXT_JOIN:
    count = 2;
    required = 2;
    names[1] = "iterable";
    break;
  case N_TEXT_REPLACE:
    count = 4;
    required = 3;
    names[1] = "old";
    names[2] = "new";
    names[3] = "count";
    defaults[3] = ca_int(-1);
    break;
  case N_TEXT_STRIP:
  case N_TEXT_LSTRIP:
  case N_TEXT_RSTRIP:
    count = 2;
    names[1] = "chars";
    break;
  case N_TEXT_STARTSWITH:
  case N_TEXT_ENDSWITH:
  case N_TEXT_FIND:
  case N_TEXT_COUNT:
    count = 4;
    required = 2;
    defaults[2] = ca_int(0);
    break;
  case N_TEXT_SPLIT:
  case N_TEXT_RSplit:
    count = 3;
    names[1] = "sep";
    names[2] = "maxsplit";
    defaults[2] = ca_int(-1);
    break;
  default:
    break;
  }
  if (!ca_native_arguments(rt, n, nkw, args, keys, count, required, names,
                           defaults, out))
    return CA_UNBOUND;
  int bytes = out[0].tag == CA_BYTES_T;
  if (!bytes && out[0].tag != CA_STR_T) {
    ca_error(rt, "TypeError", "text method requires str or bytes");
    return CA_UNBOUND;
  }
  CaString *s = (CaString *)out[0].as.obj;
  if (kind == N_TEXT_ENCODE || kind == N_TEXT_DECODE) {
    if ((kind == N_TEXT_ENCODE && bytes) || (kind == N_TEXT_DECODE && !bytes)) {
      ca_error(rt, "AttributeError",
               "encoding method is not supported on this type");
      return CA_UNBOUND;
    }
    int encoding = codec(rt, out[1]), handling = errors(rt, out[2]);
    if (!encoding || handling < 0)
      return CA_UNBOUND;
    char *buffer = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&buffer, &length);
    if (!stream) {
      ca_error(rt, "MemoryError", "codec allocation failed");
      return CA_UNBOUND;
    }
    size_t at = 0;
    if (kind == N_TEXT_DECODE && encoding == 2 && s->length >= 3 &&
        !memcmp(s->data, "\xef\xbb\xbf", 3))
      at = 3;
    if (kind == N_TEXT_ENCODE && encoding == 2)
      fwrite("\xef\xbb\xbf", 1, 3, stream);
    while (at < s->length && !rt->error) {
      size_t span = 1;
      int valid = encoding == 3 ? (unsigned char)s->data[at] < 128
                                : utf8((const unsigned char *)s->data + at,
                                       s->length - at, &span);
      if (kind == N_TEXT_ENCODE) {
        span = advance(s->data, s->length, at, 0) - at;
        valid = encoding != 3 || (unsigned char)s->data[at] < 128;
      }
      if (valid)
        fwrite(s->data + at, 1, span, stream);
      else if (!handling)
        ca_error(rt,
                 kind == N_TEXT_ENCODE ? "UnicodeEncodeError"
                                       : "UnicodeDecodeError",
                 "native text codec encountered invalid data");
      else if (handling == 2)
        fwrite(kind == N_TEXT_ENCODE ? "?" : "\xef\xbf\xbd", 1,
               kind == N_TEXT_ENCODE ? 1 : 3, stream);
      at += span;
    }
    return from_stream(rt, stream, &buffer, &length, kind == N_TEXT_ENCODE);
  }
  if (kind == N_TEXT_JOIN) {
    CaValue roots[] = {out[0], ca_collection_builtin(rt, 4, 1, 0, &out[1])};
    CaFrame frame;
    ca_push(rt, &frame, roots, 2);
    if (rt->error) {
      ca_pop(rt, &frame);
      return CA_UNBOUND;
    }
    CaContainer *c = (CaContainer *)roots[1].as.obj;
    size_t total = 0;
    for (size_t i = 0; i < c->length; i++) {
      if (c->items[i].tag != out[0].tag) {
        ca_error(rt, "TypeError", "join item has incorrect type");
        break;
      }
      size_t len = ((CaString *)c->items[i].as.obj)->length;
      if (len > SIZE_MAX - total) {
        ca_error(rt, "MemoryError", "joined value too large");
        break;
      }
      total += len;
    }
    if (c->length > 1 && s->length > (SIZE_MAX - total) / (c->length - 1))
      ca_error(rt, "MemoryError", "joined value too large");
    if (rt->error) {
      ca_pop(rt, &frame);
      return CA_UNBOUND;
    }
    if (c->length)
      total += s->length * (c->length - 1);
    CaValue value = bytes ? ca_bytes(rt, NULL, total) : ca_string(rt, "", 0);
    if (!bytes) {
      CaString *target = ca_allocate(rt, sizeof *target + total + 1, CA_STR_T);
      if (target) {
        target->length = total;
        value = (CaValue){.tag = CA_STR_T, .as.obj = (CaObj *)target};
      }
    }
    if (!rt->error) {
      CaString *target = (CaString *)value.as.obj;
      size_t at = 0;
      for (size_t i = 0; i < c->length; i++) {
        if (i) {
          memcpy(target->data + at, s->data, s->length);
          at += s->length;
        }
        CaString *part = (CaString *)c->items[i].as.obj;
        memcpy(target->data + at, part->data, part->length);
        at += part->length;
      }
    }
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : value;
  }
  if (kind == N_TEXT_LOWER || kind == N_TEXT_UPPER) {
    char *buffer = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&buffer, &length);
    if (!stream) {
      ca_error(rt, "MemoryError", "case conversion allocation failed");
      return CA_UNBOUND;
    }
    int preceding_cased = 0;
    for (size_t at = 0; at < s->length;) {
      size_t next = advance(s->data, s->length, at, bytes);
      uint32_t cp = point(s->data, at, next);
      if (bytes) {
        fputc(kind == N_TEXT_LOWER ? (cp >= 'A' && cp <= 'Z' ? cp + 32 : cp)
                                   : (cp >= 'a' && cp <= 'z' ? cp - 32 : cp),
              stream);
      } else if (kind == N_TEXT_LOWER && cp == 0x3a3) {
        int following_cased = 0;
        for (size_t look = next; look < s->length;) {
          size_t end = advance(s->data, s->length, look, 0);
          uint32_t c = point(s->data, look, end);
          if (!ca_unicode_ignorable(c)) {
            following_cased = ca_unicode_cased(c);
            break;
          }
          look = end;
        }
        fwrite(preceding_cased && !following_cased ? "\xcf\x82" : "\xcf\x83", 1,
               2, stream);
      } else {
        size_t nbytes = 0;
        const char *mapped = ca_unicode_case(cp, kind == N_TEXT_UPPER, &nbytes);
        if (mapped)
          fwrite(mapped, 1, nbytes, stream);
        else
          fwrite(s->data + at, 1, next - at, stream);
      }
      if (!ca_unicode_ignorable(cp))
        preceding_cased = ca_unicode_cased(cp);
      at = next;
    }
    return from_stream(rt, stream, &buffer, &length, bytes);
  }
  if (kind == N_TEXT_STRIP || kind == N_TEXT_LSTRIP || kind == N_TEXT_RSTRIP) {
    if (out[1].tag != CA_NONE_T && out[1].tag != out[0].tag) {
      ca_error(rt, "TypeError", "strip characters have incorrect type");
      return CA_UNBOUND;
    }
    CaString *bag = out[1].tag == CA_NONE_T ? NULL : (CaString *)out[1].as.obj;
    size_t start = 0, end = s->length;
    if (kind != N_TEXT_RSTRIP)
      while (start < end) {
        size_t next = advance(s->data, end, start, bytes);
        if (bag ? !memmem(bag->data, bag->length, s->data + start, next - start)
                : !space(s, start, next, bytes))
          break;
        start = next;
      }
    if (kind != N_TEXT_LSTRIP)
      while (end > start) {
        size_t previous = retreat(s->data, end, bytes);
        if (bag ? !memmem(bag->data, bag->length, s->data + previous,
                          end - previous)
                : !space(s, previous, end, bytes))
          break;
        end = previous;
      }
    return fragment(rt, out[0], start, end);
  }
  if (kind == N_TEXT_REPLACE) {
    if (out[1].tag != out[0].tag || out[2].tag != out[0].tag) {
      ca_error(rt, "TypeError", "replace arguments have incorrect type");
      return CA_UNBOUND;
    }
    int64_t limit;
    if (!index_arg(rt, out[3], &limit))
      return CA_UNBOUND;
    if (!limit)
      return out[0];
    CaString *old = (CaString *)out[1].as.obj,
             *replacement = (CaString *)out[2].as.obj;
    char *buffer = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&buffer, &length);
    if (!stream) {
      ca_error(rt, "MemoryError", "replacement allocation failed");
      return CA_UNBOUND;
    }
    size_t at = 0;
    int64_t replaced = 0;
    if (!old->length) {
      for (;;) {
        if (limit < 0 || replaced < limit) {
          fwrite(replacement->data, 1, replacement->length, stream);
          replaced++;
        }
        if (at >= s->length)
          break;
        size_t next = advance(s->data, s->length, at, bytes);
        fwrite(s->data + at, 1, next - at, stream);
        at = next;
      }
    } else {
      while (at < s->length && (limit < 0 || replaced < limit)) {
        const char *found =
            memmem(s->data + at, s->length - at, old->data, old->length);
        if (!found)
          break;
        size_t where = (size_t)(found - s->data);
        fwrite(s->data + at, 1, where - at, stream);
        fwrite(replacement->data, 1, replacement->length, stream);
        at = where + old->length;
        replaced++;
      }
      fwrite(s->data + at, 1, s->length - at, stream);
    }
    return from_stream(rt, stream, &buffer, &length, bytes);
  }
  if (kind == N_TEXT_SPLIT || kind == N_TEXT_RSplit) {
    if (out[1].tag != CA_NONE_T && out[1].tag != out[0].tag) {
      ca_error(rt, "TypeError", "separator has incorrect type");
      return CA_UNBOUND;
    }
    CaString *sep = out[1].tag == CA_NONE_T ? NULL : (CaString *)out[1].as.obj;
    int64_t limit;
    if (!index_arg(rt, out[2], &limit))
      return CA_UNBOUND;
    if (sep && !sep->length) {
      ca_error(rt, "ValueError", "empty separator");
      return CA_UNBOUND;
    }
    CaValue list = ca_container(rt, CA_LIST_T, 0, NULL);
    if (rt->error)
      return CA_UNBOUND;
    size_t start = 0, end = s->length;
    int64_t splits = 0;
    if (kind == N_TEXT_SPLIT) {
      if (!sep)
        while (start < end) {
          size_t next = advance(s->data, end, start, bytes);
          if (!space(s, start, next, bytes))
            break;
          start = next;
        }
      while (start < end || sep) {
        if (limit >= 0 && splits >= limit) {
          ca_append(rt, list, fragment(rt, out[0], start, end));
          break;
        }
        size_t stop = start, next = start;
        if (sep) {
          const char *found =
              memmem(s->data + start, end - start, sep->data, sep->length);
          if (!found) {
            ca_append(rt, list, fragment(rt, out[0], start, end));
            break;
          }
          stop = (size_t)(found - s->data);
          next = stop + sep->length;
        } else {
          while (stop < end) {
            next = advance(s->data, end, stop, bytes);
            if (space(s, stop, next, bytes))
              break;
            stop = next;
          }
          next = stop;
          while (next < end) {
            size_t after = advance(s->data, end, next, bytes);
            if (!space(s, next, after, bytes))
              break;
            next = after;
          }
        }
        ca_append(rt, list, fragment(rt, out[0], start, stop));
        if (rt->error)
          break;
        start = next;
        splits++;
        if (!sep && start == end)
          break;
      }
    } else {
      if (!sep)
        while (end > start) {
          size_t previous = retreat(s->data, end, bytes);
          if (!space(s, previous, end, bytes))
            break;
          end = previous;
        }
      while (end > start || sep) {
        if (limit >= 0 && splits >= limit) {
          ca_append(rt, list, fragment(rt, out[0], start, end));
          break;
        }
        size_t stop = end, next = end;
        if (sep) {
          int found = 0;
          if (end - start >= sep->length) {
            stop = end - sep->length;
            for (;;) {
              if (!memcmp(s->data + stop, sep->data, sep->length)) {
                found = 1;
                break;
              }
              if (stop == start)
                break;
              stop--;
            }
          }
          if (!found) {
            ca_append(rt, list, fragment(rt, out[0], start, end));
            break;
          }
          next = stop;
          stop += sep->length;
        } else {
          while (stop > start) {
            size_t previous = retreat(s->data, stop, bytes);
            if (space(s, previous, stop, bytes))
              break;
            stop = previous;
          }
          next = stop;
          while (next > start) {
            size_t previous = retreat(s->data, next, bytes);
            if (!space(s, previous, next, bytes))
              break;
            next = previous;
          }
        }
        ca_append(rt, list, fragment(rt, out[0], stop, end));
        if (rt->error)
          break;
        end = next;
        splits++;
        if (!sep && end == start)
          break;
      }
      CaContainer *c = (CaContainer *)list.as.obj;
      for (size_t i = 0; i < c->length / 2; i++) {
        CaValue v = c->items[i];
        c->items[i] = c->items[c->length - i - 1];
        c->items[c->length - i - 1] = v;
      }
    }
    return rt->error ? CA_UNBOUND : list;
  }
  if (kind == N_TEXT_STARTSWITH || kind == N_TEXT_ENDSWITH ||
      kind == N_TEXT_FIND || kind == N_TEXT_COUNT) {
    int64_t begin = 0, finish = INT64_MAX;
    if (!index_arg(rt, out[2], &begin) ||
        (out[3].tag != CA_NONE_T && !index_arg(rt, out[3], &finish)))
      return CA_UNBOUND;
    size_t countpoints = 0;
    for (size_t i = 0; i < s->length; i = advance(s->data, s->length, i, bytes))
      countpoints++;
    if (begin < 0) {
      begin += (int64_t)countpoints;
      if (begin < 0)
        begin = 0;
    }
    if (finish < 0) {
      finish += (int64_t)countpoints;
      if (finish < 0)
        finish = 0;
    }
    if (finish > (int64_t)countpoints)
      finish = (int64_t)countpoints;
    int valid = begin <= (int64_t)countpoints && finish >= begin;
    size_t start = 0, end = 0;
    for (int64_t i = 0; i < begin && start < s->length; i++)
      start = advance(s->data, s->length, start, bytes);
    for (int64_t i = 0; i < finish && end < s->length; i++)
      end = advance(s->data, s->length, end, bytes);
    if (out[1].tag == CA_TUPLE_T &&
        (kind == N_TEXT_STARTSWITH || kind == N_TEXT_ENDSWITH)) {
      CaContainer *tuple = (CaContainer *)out[1].as.obj;
      for (size_t i = 0; i < tuple->length; i++) {
        if (tuple->items[i].tag != out[0].tag) {
          ca_error(rt, "TypeError", "prefix has incorrect type");
          return CA_UNBOUND;
        }
        CaString *prefix = (CaString *)tuple->items[i].as.obj;
        if (valid && prefix->length <= end - start &&
            !memcmp(s->data + (kind == N_TEXT_STARTSWITH
                                   ? start
                                   : end - prefix->length),
                    prefix->data, prefix->length))
          return ca_bool(1);
      }
      return ca_bool(0);
    }
    if (out[1].tag != out[0].tag) {
      ca_error(rt, "TypeError", "substring has incorrect type");
      return CA_UNBOUND;
    }
    CaString *part = (CaString *)out[1].as.obj;
    if (kind == N_TEXT_STARTSWITH || kind == N_TEXT_ENDSWITH)
      return ca_bool(
          valid && part->length <= end - start &&
          !memcmp(s->data +
                      (kind == N_TEXT_STARTSWITH ? start : end - part->length),
                  part->data, part->length));
    if (!valid)
      return ca_int(kind == N_TEXT_FIND ? -1 : 0);
    if (!part->length)
      return ca_int(kind == N_TEXT_FIND ? begin : finish - begin + 1);
    if (kind == N_TEXT_FIND) {
      const char *found =
          memmem(s->data + start, end - start, part->data, part->length);
      if (!found)
        return ca_int(-1);
      size_t position = start;
      int64_t index = begin;
      while (position < (size_t)(found - s->data)) {
        position = advance(s->data, s->length, position, bytes);
        index++;
      }
      return ca_int(index);
    }
    int64_t matches = 0;
    while (start <= end && part->length <= end - start) {
      const char *found =
          memmem(s->data + start, end - start, part->data, part->length);
      if (!found)
        break;
      matches++;
      start = (size_t)(found - s->data) + part->length;
    }
    return ca_int(matches);
  }
  ca_error(rt, "NotImplementedError", "native text method is not implemented");
  return CA_UNBOUND;
}
