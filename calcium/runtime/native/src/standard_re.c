#include "dependencies.h"
#ifdef CA_WITH_PCRE2
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
static int isregex(CaValue v) {
  return v.tag == CA_FOREIGN_T && ((CaForeign *)v.as.obj)->kind == CA_F_REGEX;
}
static size_t bytepos(CaString *s, long n, int bytes) {
  if (n < 0)
    n = 0;
  size_t at = 0;
  while (at < s->length && n--) {
    at++;
    if (!bytes)
      while (at < s->length && ((unsigned char)s->data[at] & 192) == 128)
        at++;
  }
  return at;
}
static long charpos(CaString *s, size_t n, int bytes) {
  long count = 0;
  for (size_t i = 0; i < n && i < s->length; i++)
    if (bytes || ((unsigned char)s->data[i] & 192) != 128)
      count++;
  return count;
}
static CaValue compile(CaRuntime *rt, CaValue pattern, CaValue flags) {
  if (isregex(pattern)) {
    if (flags.tag != CA_INT_T || flags.as.i)
      return ca_dep_error(rt, "ValueError",
                          "cannot process flags with a compiled pattern");
    return pattern;
  }
  if (pattern.tag != CA_STR_T && pattern.tag != CA_BYTES_T)
    return ca_dep_error(rt, "TypeError",
                        "first argument must be string or compiled pattern");
  if (flags.tag == CA_BOOL_T)
    flags = ca_int(flags.as.i);
  if (flags.tag != CA_INT_T)
    return ca_dep_error(rt, "TypeError", "regex flags must be integers");
  if ((flags.as.i & 32) && (flags.as.i & 256))
    return ca_dep_error(rt, "ValueError",
                        "ASCII and UNICODE flags are incompatible");
  if (flags.as.i & ~(2 | 8 | 16 | 32 | 64 | 256))
    return ca_dep_error(rt, "NotImplementedError",
                        "unsupported native regular expression flags");
  if (pattern.tag == CA_BYTES_T && (flags.as.i & 32))
    return ca_dep_error(rt, "ValueError",
                        "cannot use UNICODE flag with a bytes pattern");
  uint32_t options =
      pattern.tag == CA_STR_T ? PCRE2_UTF | PCRE2_NEVER_BACKSLASH_C : 0;
  if (pattern.tag == CA_STR_T && !(flags.as.i & 256))
    options |= PCRE2_UCP;
  if (flags.as.i & 2)
    options |= PCRE2_CASELESS;
  if (flags.as.i & 8)
    options |= PCRE2_MULTILINE;
  if (flags.as.i & 16)
    options |= PCRE2_DOTALL;
  if (flags.as.i & 64)
    options |= PCRE2_EXTENDED;
  CaString *s = (CaString *)pattern.as.obj;
  int error;
  PCRE2_SIZE at;
  pcre2_code *code =
      pcre2_compile((PCRE2_SPTR)s->data, s->length, options, &error, &at, NULL);
  if (!code) {
    unsigned char message[256];
    pcre2_get_error_message(error, message, sizeof message);
    return ca_dep_error(rt, "re.PatternError", (char *)message);
  }
  CaValue state = ca_container(rt, CA_DICT_T, 0, NULL),
          v = ca_native_object(rt, CA_F_REGEX, state, code,
                               pattern.tag == CA_BYTES_T);
  if (rt->error) {
    pcre2_code_free(code);
    return CA_UNBOUND;
  }
  ca_dep_put(rt, v, "pattern", pattern);
  ca_dep_put(
      rt, v, "flags",
      ca_int(flags.as.i |
             ((pattern.tag == CA_STR_T && !(flags.as.i & 256)) ? 32 : 0)));
  uint32_t groups;
  pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &groups);
  ca_dep_put(rt, v, "groups", ca_int(groups));
  return v;
}
static CaValue match(CaRuntime *rt, CaValue regex, CaValue string, long pos,
                     long end, int mode, uint32_t extra) {
  CaForeign *f = (CaForeign *)regex.as.obj;
  if (string.tag != (f->auxiliary ? CA_BYTES_T : CA_STR_T))
    return ca_dep_error(
        rt, "TypeError",
        "pattern and string must have matching text/bytes types");
  CaString *s = (CaString *)string.as.obj;
  size_t start = bytepos(s, pos, (int)f->auxiliary),
         limit = end < 0 ? 0 : bytepos(s, end, (int)f->auxiliary);
  if (start > limit)
    return CA_NONE;
  pcre2_match_data *data =
      pcre2_match_data_create_from_pattern(f->handle, NULL);
  if (!data)
    return ca_dep_error(rt, "MemoryError", "regex match allocation failed");
  uint32_t opts = extra | (mode != D_RE_SEARCH ? PCRE2_ANCHORED : 0) |
                  (mode == D_RE_FULLMATCH ? PCRE2_ENDANCHORED : 0);
  int n = pcre2_match(f->handle, (PCRE2_SPTR)s->data, limit, start, opts, data,
                      NULL);
  if (n == PCRE2_ERROR_NOMATCH) {
    pcre2_match_data_free(data);
    return CA_NONE;
  }
  if (n < 0) {
    pcre2_match_data_free(data);
    return ca_dep_error(rt, "re.PatternError",
                        "native regular expression match failed");
  }
  CaValue state = ca_container(rt, CA_DICT_T, 0, NULL),
          v = ca_native_object(rt, CA_F_MATCH, state, data, 0);
  if (rt->error) {
    pcre2_match_data_free(data);
    return CA_UNBOUND;
  }
  ca_dep_put(rt, v, "re", regex);
  ca_dep_put(rt, v, "string", string);
  ca_dep_put(rt, v, "pos", ca_int(pos < 0 ? 0 : pos));
  ca_dep_put(rt, v, "endpos", ca_int(charpos(s, limit, (int)f->auxiliary)));
  return v;
}
static int group_index(CaRuntime *rt, CaValue v, CaValue key) {
  CaForeign *regex = (CaForeign *)ca_dep_get(v, "re").as.obj;
  int index = -1;
  if (key.tag == CA_INT_T)
    index = key.as.i >= 0 && key.as.i <= INT_MAX ? (int)key.as.i : -1;
  else if (key.tag == CA_STR_T) {
    const char *name = ca_native_text(rt, key, 0);
    if (name)
      index = pcre2_substring_number_from_name(regex->handle, (PCRE2_SPTR)name);
  }
  if (index < 0 || index > ca_dep_get(ca_dep_get(v, "re"), "groups").as.i) {
    ca_error(rt, "IndexError", "no such group");
    return -1;
  }
  return index;
}
static CaValue group(CaRuntime *rt, CaValue v, int index, CaValue missing) {
  CaValue string = ca_dep_get(v, "string");
  CaString *s = (CaString *)string.as.obj;
  PCRE2_SIZE *off = pcre2_get_ovector_pointer(((CaForeign *)v.as.obj)->handle);
  if (off[index * 2] == PCRE2_UNSET)
    return missing;
  return string.tag == CA_STR_T ? ca_string(rt, s->data + off[index * 2],
                                            off[index * 2 + 1] - off[index * 2])
                                : ca_bytes(rt, s->data + off[index * 2],
                                           off[index * 2 + 1] - off[index * 2]);
}
static CaValue expand(CaRuntime *rt, CaValue m, CaValue replacement) {
  if (replacement.tag != ca_dep_get(m, "string").tag)
    return ca_dep_error(rt, "TypeError",
                        "replacement must have matching text/bytes type");
  CaString *s = (CaString *)replacement.as.obj;
  CaValue roots[] = {m, replacement, ca_container(rt, CA_LIST_T, 0, NULL),
                     CA_UNBOUND};
  CaFrame f;
  ca_push(rt, &f, roots, 4);
  size_t start = 0;
  for (size_t i = 0; i < s->length && !rt->error; i++)
    if (s->data[i] == '\\') {
      roots[3] = replacement.tag == CA_STR_T
                     ? ca_string(rt, s->data + start, i - start)
                     : ca_bytes(rt, s->data + start, i - start);
      ca_append(rt, roots[2], roots[3]);
      if (++i == s->length) {
        ca_error(rt, "re.PatternError", "bad escape at end of replacement");
        break;
      }
      char c = s->data[i];
      CaValue key = CA_UNBOUND;
      if (c == 'g' && i + 1 < s->length && s->data[i + 1] == '<') {
        size_t begin = i + 2;
        i = begin;
        while (i < s->length && s->data[i] != '>')
          i++;
        if (i == s->length) {
          ca_error(rt, "re.PatternError", "unterminated group reference");
          break;
        }
        long index = 0;
        int digits = 1;
        for (size_t j = begin; j < i; j++)
          if (s->data[j] < '0' || s->data[j] > '9')
            digits = 0;
          else if (index < 100000)
            index = index * 10 + s->data[j] - '0';
        key =
            digits ? ca_int(index) : ca_string(rt, s->data + begin, i - begin);
      } else if (c >= '1' && c <= '9') {
        int index = c - '0';
        if (i + 1 < s->length && s->data[i + 1] >= '0' && s->data[i + 1] <= '9')
          index = index * 10 + s->data[++i] - '0';
        key = ca_int(index);
      }
      if (key.tag != CA_UNBOUND_T) {
        int index = group_index(rt, m, key);
        roots[3] = index < 0 ? CA_UNBOUND
                             : group(rt, m, index,
                                     replacement.tag == CA_STR_T
                                         ? ca_string(rt, "", 0)
                                         : ca_bytes(rt, "", 0));
      } else {
        char actual = c == 'n'    ? '\n'
                      : c == 'r'  ? '\r'
                      : c == 't'  ? '\t'
                      : c == '\\' ? '\\'
                                  : 0;
        if (!actual) {
          ca_error(rt, "NotImplementedError",
                   "unsupported native replacement escape");
          break;
        }
        roots[3] = replacement.tag == CA_STR_T ? ca_string(rt, &actual, 1)
                                               : ca_bytes(rt, &actual, 1);
      }
      if (!rt->error)
        ca_append(rt, roots[2], roots[3]);
      start = i + 1;
    }
  if (!rt->error) {
    roots[3] = replacement.tag == CA_STR_T
                   ? ca_string(rt, s->data + start, s->length - start)
                   : ca_bytes(rt, s->data + start, s->length - start);
    ca_append(rt, roots[2], roots[3]);
  }
  roots[3] =
      replacement.tag == CA_STR_T ? ca_string(rt, "", 0) : ca_bytes(rt, "", 0);
  CaValue args[] = {roots[3], roots[2]};
  CaValue out =
      rt->error ? CA_UNBOUND : ca_text_call(rt, N_TEXT_JOIN, 2, 0, args, NULL);
  ca_pop(rt, &f);
  return out;
}
CaValue ca_re_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                   const char **keys) {
  if (id >= D_RE_GROUP) {
    if (n < 1 || a[0].tag != CA_FOREIGN_T ||
        ((CaForeign *)a[0].as.obj)->kind != CA_F_MATCH)
      return ca_dep_error(rt, "TypeError", "expected match object");
    CaValue m = a[0];
    int groups = (int)ca_dep_get(ca_dep_get(m, "re"), "groups").as.i;
    if (id == D_RE_EXPAND) {
      if (n != 2 || nkw)
        goto bad;
      return expand(rt, m, a[1]);
    }
    if (id == D_RE_GROUP) {
      if (nkw)
        goto bad;
      if (n <= 2) {
        int index = group_index(rt, m, n == 2 ? a[1] : ca_int(0));
        return index < 0 ? CA_UNBOUND : group(rt, m, index, CA_NONE);
      }
      CaValue out = ca_container(rt, CA_TUPLE_T, (size_t)n - 1, NULL);
      for (int i = 1; i < n && !rt->error; i++) {
        int index = group_index(rt, m, a[i]);
        if (index >= 0)
          ((CaContainer *)out.as.obj)->items[i - 1] =
              group(rt, m, index, CA_NONE);
      }
      return rt->error ? CA_UNBOUND : out;
    }
    if (id == D_RE_GROUPS || id == D_RE_GROUPDICT) {
      CaValue out[2], defaults[] = {CA_NONE, CA_NONE};
      const char *names[] = {"self", "default"};
      if (!ca_native_arguments(rt, n, nkw, a, keys, 2, 1, names, defaults, out))
        return CA_UNBOUND;
      if (id == D_RE_GROUPS) {
        CaValue tuple = ca_container(rt, CA_TUPLE_T, (size_t)groups, NULL);
        for (int i = 1; i <= groups && !rt->error; i++)
          ((CaContainer *)tuple.as.obj)->items[i - 1] = group(rt, m, i, out[1]);
        return rt->error ? CA_UNBOUND : tuple;
      }
      CaValue dict = ca_container(rt, CA_DICT_T, 0, NULL);
      CaForeign *regex = (CaForeign *)ca_dep_get(m, "re").as.obj;
      uint32_t count, size;
      PCRE2_SPTR table;
      pcre2_pattern_info(regex->handle, PCRE2_INFO_NAMECOUNT, &count);
      pcre2_pattern_info(regex->handle, PCRE2_INFO_NAMEENTRYSIZE, &size);
      pcre2_pattern_info(regex->handle, PCRE2_INFO_NAMETABLE, &table);
      for (uint32_t i = 0; i < count; i++) {
        int index = (table[0] << 8) | table[1];
        ca_namespace_set(rt, dict, (char *)table + 2,
                         group(rt, m, index, out[1]));
        table += size;
      }
      return dict;
    }
    if (n > 2 || nkw)
      goto bad;
    int index = group_index(rt, m, n == 2 ? a[1] : ca_int(0));
    if (index < 0)
      return CA_UNBOUND;
    PCRE2_SIZE *off =
        pcre2_get_ovector_pointer(((CaForeign *)m.as.obj)->handle);
    CaString *s = (CaString *)ca_dep_get(m, "string").as.obj;
    int bytes = ca_dep_get(m, "string").tag == CA_BYTES_T;
    CaValue begin = ca_int(off[index * 2] == PCRE2_UNSET
                               ? -1
                               : charpos(s, off[index * 2], bytes)),
            end = ca_int(off[index * 2 + 1] == PCRE2_UNSET
                             ? -1
                             : charpos(s, off[index * 2 + 1], bytes));
    return id == D_RE_START ? begin
           : id == D_RE_END
               ? end
               : ca_container(rt, CA_TUPLE_T, 2, (CaValue[]){begin, end});
  }
  if (id == D_RE_ESCAPE) {
    if (n != 1 || nkw || (a[0].tag != CA_STR_T && a[0].tag != CA_BYTES_T))
      goto bad;
    CaString *s = (CaString *)a[0].as.obj;
    char *buffer = malloc(s->length * 2 + 1);
    if (!buffer)
      return ca_dep_error(rt, "MemoryError", "regex escape allocation failed");
    size_t at = 0;
    for (size_t i = 0; i < s->length; i++) {
      if (strchr("()[]{}?*+-|^$\\.&~# \t\n\r\v\f", s->data[i]) && s->data[i])
        buffer[at++] = '\\';
      buffer[at++] = s->data[i];
    }
    CaValue result = a[0].tag == CA_STR_T ? ca_string(rt, buffer, at)
                                          : ca_bytes(rt, buffer, at);
    free(buffer);
    return result;
  }
  if (id == D_RE_COMPILE) {
    CaValue out[2], d[] = {CA_NONE, ca_int(0)};
    const char *names[] = {"pattern", "flags"};
    if (!ca_native_arguments(rt, n, nkw, a, keys, 2, 1, names, d, out))
      return CA_UNBOUND;
    return compile(rt, out[0], out[1]);
  }
  int bound = n && isregex(a[0]);
  CaValue out[6], defaults[] = {CA_NONE,   CA_NONE,   CA_NONE,
                                ca_int(0), ca_int(0), ca_int(-1)};
  const char *names[6];
  int total, required;
  if (bound) {
    names[0] = "self";
    names[1] = (id == D_RE_SUB || id == D_RE_SUBN) ? "repl" : "string";
    names[2] = (id == D_RE_SUB || id == D_RE_SUBN)
                   ? "string"
                   : (id == D_RE_SPLIT ? "maxsplit" : "pos");
    names[3] = (id == D_RE_SUB || id == D_RE_SUBN) ? "count" : "endpos";
    total = 4;
    required = (id == D_RE_SUB || id == D_RE_SUBN) ? 3 : 2;
    defaults[2] = ca_int(0);
    defaults[3] = ca_int(LONG_MAX);
    if (id == D_RE_SUB || id == D_RE_SUBN || id == D_RE_SPLIT)
      defaults[3] = ca_int(0);
    if (id == D_RE_SPLIT)
      total = 3;
  } else {
    names[0] = "pattern";
    names[1] = (id == D_RE_SUB || id == D_RE_SUBN) ? "repl" : "string";
    names[2] = (id == D_RE_SUB || id == D_RE_SUBN)
                   ? "string"
                   : (id == D_RE_SPLIT ? "maxsplit" : "flags");
    names[3] = (id == D_RE_SUB || id == D_RE_SUBN) ? "count" : "flags";
    names[4] = "flags";
    total =
        (id == D_RE_SUB || id == D_RE_SUBN) ? 5 : (id == D_RE_SPLIT ? 4 : 3);
    required = (id == D_RE_SUB || id == D_RE_SUBN) ? 3 : 2;
    defaults[2] = ca_int(0);
  }
  if (!ca_native_arguments(rt, n, nkw, a, keys, total, required, names,
                           defaults, out))
    return CA_UNBOUND;
  int is_sub = id == D_RE_SUB || id == D_RE_SUBN;
  if (is_sub || id == D_RE_SPLIT) {
    int at = is_sub ? 3 : 2;
    out[at] = ca_builtin_index(rt, out[at]);
    if (rt->error)
      return CA_UNBOUND;
    if (out[at].tag != CA_INT_T)
      return ca_dep_error(rt, "OverflowError",
                          "regex count exceeds platform limit");
  }
  if (bound && !is_sub && id != D_RE_SPLIT) {
    for (int i = 2; i <= 3; i++) {
      out[i] = ca_builtin_index(rt, out[i]);
      if (rt->error)
        return CA_UNBOUND;
      if (out[i].tag == CA_BIG_T)
        out[i] =
            ca_int(((CaBig *)out[i].as.obj)->sign < 0 ? LONG_MIN : LONG_MAX);
    }
  }
  CaValue flags = bound ? ca_int(0)
                        : out[is_sub             ? 4
                              : id == D_RE_SPLIT ? 3
                                                 : 2];
  CaValue roots[] = {compile(rt, out[0], flags),
                     out[is_sub ? 2 : 1],
                     is_sub ? out[1] : CA_NONE,
                     CA_UNBOUND,
                     CA_UNBOUND,
                     CA_UNBOUND};
  CaFrame f;
  ca_push(rt, &f, roots, 6);
  if (rt->error)
    goto done;
  long pos = bound && !is_sub && id != D_RE_SPLIT ? out[2].as.i : 0,
       end = bound && !is_sub && id != D_RE_SPLIT ? out[3].as.i : LONG_MAX;
  if (bound && !is_sub && id != D_RE_SPLIT &&
      (out[2].tag != CA_INT_T || out[3].tag != CA_INT_T)) {
    ca_error(rt, "TypeError", "regex positions must be integers");
    goto done;
  }
  if (id == D_RE_MATCH || id == D_RE_SEARCH || id == D_RE_FULLMATCH) {
    roots[5] = match(rt, roots[0], roots[1], pos, end, id, 0);
    goto done;
  }
  if (roots[1].tag != CA_STR_T && roots[1].tag != CA_BYTES_T) {
    ca_error(rt, "TypeError", "regex string must be text or bytes");
    goto done;
  }
  if (id == D_RE_FINDITER) {
    roots[5] = ca_native_object(rt, CA_F_REGEX_ITER,
                                ca_container(rt, CA_DICT_T, 0, NULL), NULL, 0);
    ca_dep_put(rt, roots[5], "re", roots[0]);
    ca_dep_put(rt, roots[5], "string", roots[1]);
    ca_dep_put(rt, roots[5], "pos", ca_int(pos));
    ca_dep_put(rt, roots[5], "endpos", ca_int(end));
    goto done;
  }
  roots[3] = ca_container(rt, CA_LIST_T, 0, NULL);
  CaString *s = (CaString *)roots[1].as.obj;
  int bytes = roots[1].tag == CA_BYTES_T;
  long limit = is_sub             ? out[3].as.i
               : id == D_RE_SPLIT ? out[2].as.i
                                  : 0,
       count = 0;
  size_t previous = 0;
  uint32_t extra = 0;
  for (;;) {
    roots[4] = match(rt, roots[0], roots[1], pos, end, D_RE_SEARCH, extra);
    if (rt->error)
      break;
    if (roots[4].tag == CA_NONE_T) {
      if (extra) {
        size_t next = bytepos(s, pos + 1, bytes);
        if (next > bytepos(s, pos, bytes)) {
          pos++;
          extra = 0;
          continue;
        }
      }
      break;
    }
    PCRE2_SIZE *off =
        pcre2_get_ovector_pointer(((CaForeign *)roots[4].as.obj)->handle);
    size_t begin = off[0], finish = off[1];
    if (is_sub || id == D_RE_SPLIT) {
      if (limit < 0 || (limit > 0 && count >= limit))
        break;
      CaValue prefix =
          bytes ? ca_bytes(rt, s->data + previous, begin - previous)
                : ca_string(rt, s->data + previous, begin - previous);
      ca_append(rt, roots[3], prefix);
      if (is_sub) {
        CaValue replacement =
            (roots[2].tag == CA_STR_T || roots[2].tag == CA_BYTES_T)
                ? expand(rt, roots[4], roots[2])
                : ca_call(rt, roots[2], 1, 0, &roots[4], NULL);
        if (!rt->error && replacement.tag != roots[1].tag)
          ca_error(rt, "TypeError", "replacement returned wrong type");
        if (!rt->error)
          ca_append(rt, roots[3], replacement);
      } else
        for (int g = 1; g <= ca_dep_get(roots[0], "groups").as.i && !rt->error;
             g++)
          ca_append(rt, roots[3], group(rt, roots[4], g, CA_NONE));
      previous = finish;
      count++;
    } else if (id == D_RE_FINDITER)
      ca_append(rt, roots[3], roots[4]);
    else {
      int groups = (int)ca_dep_get(roots[0], "groups").as.i;
      CaValue missing = bytes ? ca_bytes(rt, "", 0) : ca_string(rt, "", 0),
              value = CA_UNBOUND;
      if (groups <= 1)
        value = group(rt, roots[4], groups, missing);
      else {
        value = ca_container(rt, CA_TUPLE_T, (size_t)groups, NULL);
        for (int g = 1; g <= groups && !rt->error; g++)
          ((CaContainer *)value.as.obj)->items[g - 1] =
              group(rt, roots[4], g, missing);
      }
      if (!rt->error)
        ca_append(rt, roots[3], value);
    }
    if (rt->error)
      break;
    pos = charpos(s, finish, bytes);
    extra = begin == finish ? PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED : 0;
    ca_safepoint(rt);
  }
  if (!rt->error) {
    if (is_sub || id == D_RE_SPLIT) {
      CaValue tail =
          bytes ? ca_bytes(rt, s->data + previous, s->length - previous)
                : ca_string(rt, s->data + previous, s->length - previous);
      ca_append(rt, roots[3], tail);
    }
    if (is_sub) {
      roots[4] = bytes ? ca_bytes(rt, "", 0) : ca_string(rt, "", 0);
      CaValue args[] = {roots[4], roots[3]};
      roots[5] = ca_text_call(rt, N_TEXT_JOIN, 2, 0, args, NULL);
      if (id == D_RE_SUBN && !rt->error)
        roots[5] = ca_container(rt, CA_TUPLE_T, 2,
                                (CaValue[]){roots[5], ca_int(count)});
    } else
      roots[5] = id == D_RE_FINDITER ? ca_iter(rt, roots[3]) : roots[3];
  }
done:;
  CaValue result = roots[5];
  ca_pop(rt, &f);
  return rt->error ? CA_UNBOUND : result;
bad:
  return ca_dep_error(rt, "TypeError",
                      "invalid native regular expression arguments");
}
CaValue ca_re_attribute(CaRuntime *rt, CaValue v, const char *name) {
  CaValue value = ca_dep_get(v, name);
  if (value.tag != CA_UNBOUND_T)
    return value;
  const char *names[] = {"match",    "search", "fullmatch", "findall",
                         "finditer", "split",  "sub",       "subn",
                         "group",    "groups", "groupdict", "start",
                         "end",      "span",   "expand"};
  int ids[] = {D_RE_MATCH,    D_RE_SEARCH, D_RE_FULLMATCH, D_RE_FINDALL,
               D_RE_FINDITER, D_RE_SPLIT,  D_RE_SUB,       D_RE_SUBN,
               D_RE_GROUP,    D_RE_GROUPS, D_RE_GROUPDICT, D_RE_START,
               D_RE_END,      D_RE_SPAN,   D_RE_EXPAND};
  int is_match = ((CaForeign *)v.as.obj)->kind == CA_F_MATCH;
  for (int i = is_match ? 8 : 0; i < (is_match ? 15 : 8); i++)
    if (!strcmp(name, names[i]))
      return ca_native_method(rt, v, ids[i]);
  return ca_dep_error(rt, "AttributeError", name);
}
int ca_re_next(CaRuntime *rt, CaValue v, CaValue *out) {
  CaForeign *iterator = (CaForeign *)v.as.obj;
  if (iterator->auxiliary < 0)
    return 0;
  CaValue regex = ca_dep_get(v, "re"), text = ca_dep_get(v, "string");
  CaString *s = (CaString *)text.as.obj;
  long pos = ca_dep_get(v, "pos").as.i, end = ca_dep_get(v, "endpos").as.i;
  int bytes = text.tag == CA_BYTES_T;
  for (;;) {
    *out = match(rt, regex, text, pos, end, D_RE_SEARCH,
                 (uint32_t)iterator->auxiliary);
    if (rt->error)
      return -1;
    if (out->tag == CA_NONE_T) {
      if (iterator->auxiliary &&
          bytepos(s, pos + 1, bytes) > bytepos(s, pos, bytes)) {
        pos++;
        iterator->auxiliary = 0;
        continue;
      }
      iterator->auxiliary = -1;
      return 0;
    }
    PCRE2_SIZE *offset =
        pcre2_get_ovector_pointer(((CaForeign *)out->as.obj)->handle);
    iterator->auxiliary =
        offset[0] == offset[1] ? PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED : 0;
    ca_dep_put(rt, v, "pos", ca_int(charpos(s, offset[1], bytes)));
    return 1;
  }
}
void ca_re_finalize(CaForeign *f) {
  if (f->kind == CA_F_REGEX)
    pcre2_code_free(f->handle);
  if (f->kind == CA_F_MATCH)
    pcre2_match_data_free(f->handle);
}
#else
int ca_re_next(CaRuntime *rt, CaValue v, CaValue *out) {
  (void)rt;
  (void)v;
  (void)out;
  return -1;
}
CaValue ca_re_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                   const char **k) {
  (void)id;
  (void)n;
  (void)nkw;
  (void)a;
  (void)k;
  return ca_dep_error(rt, "ImportError", "native re requires PCRE2");
}
CaValue ca_re_attribute(CaRuntime *rt, CaValue v, const char *name) {
  (void)v;
  return ca_dep_error(rt, "AttributeError", name);
}
void ca_re_finalize(CaForeign *f) { (void)f; }
#endif
