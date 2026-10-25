#include "standard.h"
#include <ctype.h>
/* Native argparse definitions remain traced values, including type callables.
 */
enum { DESCRIPTION, PROG, DEFS, NAMESPACE, ADD_HELP, ABBREV };
enum {
  FLAGS,
  DEST,
  ACTION,
  NARGS,
  DEFAULT,
  TYPE,
  REQUIRED,
  CHOICES,
  HELP,
  METAVAR,
  DEF_COUNT
};
static CaValue *fields(CaValue v) {
  return ((CaContainer *)((CaForeign *)v.as.obj)->state.as.obj)->items;
}
static const char *text(CaValue v) { return ((CaString *)v.as.obj)->data; }
static int kind(CaValue v, int k) {
  return v.tag == CA_FOREIGN_T && ((CaForeign *)v.as.obj)->kind == k;
}
static void usage(CaRuntime *rt, CaValue parser, FILE *stream) {
  CaValue *p = fields(parser);
  fprintf(stream, "usage: %s", text(p[PROG]));
  if (ca_truth(rt, p[ADD_HELP]))
    fputs(" [-h]", stream);
  CaContainer *defs = (CaContainer *)p[DEFS].as.obj;
  for (size_t i = 0; i < defs->length; i++) {
    CaValue *d = fields(defs->items[i]);
    CaContainer *flags = (CaContainer *)d[FLAGS].as.obj;
    int optional = text(flags->items[0])[0] == '-';
    if (optional)
      fputs(" [", stream);
    else
      fputc(' ', stream);
    fputs(optional ? text(flags->items[0]) : text(d[DEST]), stream);
    if (optional && !strcmp(text(d[ACTION]), "store"))
      fprintf(stream, " %s",
              d[METAVAR].tag == CA_STR_T ? text(d[METAVAR]) : text(d[DEST]));
    if (d[NARGS].tag == CA_STR_T && strcmp(text(d[NARGS]), "?"))
      fputs(" ...", stream);
    if (optional)
      fputc(']', stream);
  }
  fputc('\n', stream);
}
static CaValue exit_code(CaRuntime *rt, int code) {
  CaValue value = ca_int(code);
  CaValue error = ca_exception_new(rt, ca_type(CA_T_SYSTEMEXIT), 1, &value);
  if (!rt->error)
    ca_raise(rt, error, CA_NONE, 0);
  return CA_UNBOUND;
}
static CaValue parse_error(CaRuntime *rt, CaValue parser, const char *message) {
  usage(rt, parser, stderr);
  fprintf(stderr, "%s: error: %s\n", text(fields(parser)[PROG]), message);
  return exit_code(rt, 2);
}
static CaValue help(CaRuntime *rt, CaValue parser, int exiting) {
  usage(rt, parser, stdout);
  CaValue *p = fields(parser);
  if (p[DESCRIPTION].tag == CA_STR_T)
    fprintf(stdout, "\n%s\n", text(p[DESCRIPTION]));
  fputs("\noptions:\n", stdout);
  if (ca_truth(rt, p[ADD_HELP]))
    fputs("  -h, --help  show this help message and exit\n", stdout);
  CaContainer *defs = (CaContainer *)p[DEFS].as.obj;
  for (size_t i = 0; i < defs->length; i++) {
    CaValue *d = fields(defs->items[i]);
    CaContainer *flags = (CaContainer *)d[FLAGS].as.obj;
    for (size_t j = 0; j < flags->length; j++)
      fprintf(stdout, "%s%s", j ? ", " : "  ", text(flags->items[j]));
    if (d[HELP].tag == CA_STR_T)
      fprintf(stdout, "  %s", text(d[HELP]));
    fputc('\n', stdout);
  }
  return exiting ? exit_code(rt, 0) : CA_NONE;
}
static CaValue convert(CaRuntime *rt, CaValue parser, CaValue definition,
                       CaValue value) {
  CaValue *d = fields(definition);
  CaValue result = d[TYPE].tag == CA_NONE_T
                       ? value
                       : ca_call(rt, d[TYPE], 1, 0, &value, NULL);
  if (rt->error) {
    CaValue error = ca_exception_take(rt);
    CaFrame root;
    ca_push(rt, &root, &error, 1);
    if (ca_exception_match(rt, error, ca_type(CA_T_TYPEERROR)) ||
        ca_exception_match(rt, error, ca_type(CA_T_VALUEERROR))) {
      char msg[256];
      snprintf(msg, sizeof msg, "argument %s: invalid value: '%s'",
               text(d[DEST]), text(value));
      parse_error(rt, parser, msg);
    } else
      ca_error_set(rt, error);
    ca_pop(rt, &root);
    return CA_UNBOUND;
  }
  CaFrame root;
  ca_push(rt, &root, &result, 1);
  if (d[CHOICES].tag != CA_NONE_T && !ca_contains(rt, d[CHOICES], result)) {
    if (!rt->error) {
      char msg[256];
      snprintf(msg, sizeof msg, "argument %s: invalid choice: '%s'",
               text(d[DEST]), text(value));
      parse_error(rt, parser, msg);
    }
  }
  ca_pop(rt, &root);
  return rt->error ? CA_UNBOUND : result;
}
static int matching(CaRuntime *rt, CaValue parser, const char *name,
                    size_t length) {
  CaValue *p = fields(parser);
  CaContainer *defs = (CaContainer *)p[DEFS].as.obj;
  int match = -1, ambiguous = 0;
  for (size_t i = 0; i < defs->length; i++) {
    CaContainer *flags = (CaContainer *)fields(defs->items[i])[FLAGS].as.obj;
    for (size_t j = 0; j < flags->length; j++) {
      const char *flag = text(flags->items[j]);
      if (flag[0] != '-')
        continue;
      if (strlen(flag) == length && !memcmp(flag, name, length))
        return (int)i;
      if (p[ABBREV].as.i && length > 2 && name[1] == '-' &&
          !strncmp(flag, name, length)) {
        if (match >= 0 && match != (int)i)
          ambiguous = 1;
        match = (int)i;
      }
    }
  }
  if (ambiguous) {
    parse_error(rt, parser, "ambiguous option");
    return -2;
  }
  return match;
}
static int option(CaRuntime *rt, CaValue parser, const char *token) {
  if (token[0] != '-' || !token[1])
    return 0;
  if (token[1] != '-' && (isdigit((unsigned char)token[1]) || token[1] == '.'))
    return 0;
  (void)rt;
  (void)parser;
  return 1;
}
static CaValue parse(CaRuntime *rt, CaValue parser, CaValue input, CaValue ns) {
  CaValue roots[] = {parser,     ns,         CA_UNBOUND,
                     CA_UNBOUND, CA_UNBOUND, CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 6);
  if (input.tag == CA_NONE_T) {
    CaContainer *argv = (CaContainer *)rt->arguments.as.obj;
    roots[2] = ca_container(rt, CA_LIST_T, argv->length ? argv->length - 1 : 0,
                            argv->length ? argv->items + 1 : NULL);
  } else
    roots[2] = ca_collection_builtin(rt, 4, 1, 0, &input);
  CaValue *p = fields(parser);
  CaContainer *defs = (CaContainer *)p[DEFS].as.obj;
  if (!rt->error && roots[1].tag == CA_NONE_T)
    roots[1] = ca_object_call(rt, p[NAMESPACE], 0, 0, NULL, NULL);
  roots[3] = ca_container(rt, CA_LIST_T, 0, NULL);
  size_t definition_count = defs->length;
  unsigned char *seen = calloc(definition_count ? definition_count : 1, 1);
  if (!seen)
    ca_error(rt, "MemoryError", "argument parser allocation failed");
  for (size_t i = 0; i < defs->length && !rt->error; i++) {
    CaValue *d = fields(defs->items[i]);
    ca_set_attribute(rt, roots[1], text(d[DEST]), d[DEFAULT]);
  }
  CaContainer *tokens = rt->error ? NULL : (CaContainer *)roots[2].as.obj;
  int positional_only = 0;
  for (size_t i = 0; tokens && i < tokens->length && !rt->error; i++) {
    if (defs->length != definition_count) {
      ca_error(rt, "RuntimeError",
               "argument definitions changed during parsing");
      break;
    }
    if (tokens->items[i].tag != CA_STR_T) {
      ca_error(rt, "TypeError", "arguments must be strings");
      break;
    }
    const char *token = text(tokens->items[i]);
    if (!positional_only && !strcmp(token, "--")) {
      positional_only = 1;
      continue;
    }
    if (!positional_only && p[ADD_HELP].as.i &&
        (!strcmp(token, "-h") || !strcmp(token, "--help"))) {
      help(rt, parser, 1);
      break;
    }
    if (positional_only || !option(rt, parser, token)) {
      ca_append(rt, roots[3], tokens->items[i]);
      continue;
    }
    const char *equals = strchr(token, '=');
    size_t length = equals ? (size_t)(equals - token) : strlen(token);
    int index = matching(rt, parser, token, length);
    if (index < 0) {
      if (!rt->error) {
        char msg[256];
        snprintf(msg, sizeof msg, "unrecognized arguments: %s", token);
        parse_error(rt, parser, msg);
      }
      break;
    }
    CaValue definition = defs->items[index];
    CaValue *d = fields(definition);
    seen[index] = 1;
    const char *action = text(d[ACTION]);
    if (strcmp(action, "store")) {
      if (equals) {
        parse_error(rt, parser, "flag does not take a value");
        break;
      }
      CaValue value = ca_bool(!strcmp(action, "store_true"));
      if (!strcmp(action, "count")) {
        CaValue old = ca_attribute(rt, roots[1], text(d[DEST]), 0);
        value = old.tag == CA_NONE_T ? ca_int(1)
                                     : ca_numeric(rt, old, ca_int(1), '+');
      }
      ca_set_attribute(rt, roots[1], text(d[DEST]), value);
      continue;
    }
    size_t count = 1;
    int variable = d[NARGS].tag == CA_STR_T && strcmp(text(d[NARGS]), "?");
    if (d[NARGS].tag == CA_INT_T)
      count = (size_t)d[NARGS].as.i;
    if (variable)
      count = SIZE_MAX;
    int scalar = d[NARGS].tag == CA_NONE_T ||
                 (d[NARGS].tag == CA_STR_T && !strcmp(text(d[NARGS]), "?"));
    roots[4] = scalar ? CA_NONE : ca_container(rt, CA_LIST_T, 0, NULL);
    size_t consumed = 0;
    while (consumed < count && !rt->error) {
      CaValue value;
      if (!consumed && equals)
        value = ca_string(rt, equals + 1, strlen(equals + 1));
      else if (i + 1 < tokens->length && tokens->items[i + 1].tag == CA_STR_T &&
               !option(rt, parser, text(tokens->items[i + 1])))
        value = tokens->items[++i];
      else
        break;
      roots[5] = convert(rt, parser, definition, value);
      if (rt->error)
        break;
      if (scalar)
        roots[4] = roots[5];
      else
        ca_append(rt, roots[4], roots[5]);
      consumed++;
    }
    if (!rt->error &&
        ((!consumed &&
          !(d[NARGS].tag == CA_STR_T && strchr("?*", text(d[NARGS])[0]))) ||
         (!variable && consumed < count &&
          !(d[NARGS].tag == CA_STR_T && !strcmp(text(d[NARGS]), "?")))))
      parse_error(rt, parser, "expected argument value");
    if (!rt->error)
      ca_set_attribute(rt, roots[1], text(d[DEST]), roots[4]);
  }
  CaContainer *positionals = (CaContainer *)roots[3].as.obj;
  size_t at = 0;
  for (size_t index = 0; index < defs->length && !rt->error; index++) {
    if (defs->length != definition_count) {
      ca_error(rt, "RuntimeError",
               "argument definitions changed during parsing");
      break;
    }
    CaValue definition = defs->items[index];
    CaValue *d = fields(definition);
    CaContainer *flags = (CaContainer *)d[FLAGS].as.obj;
    if (text(flags->items[0])[0] == '-') {
      if (d[REQUIRED].as.i && !seen[index])
        parse_error(rt, parser, "required option is missing");
      continue;
    }
    int scalar = d[NARGS].tag == CA_NONE_T ||
                 (d[NARGS].tag == CA_STR_T && !strcmp(text(d[NARGS]), "?"));
    size_t count = d[NARGS].tag == CA_INT_T ? (size_t)d[NARGS].as.i
                   : scalar                 ? 1
                                            : positionals->length - at;
    if (!scalar && d[NARGS].tag == CA_STR_T) {
      size_t reserve = 0;
      for (size_t j = index + 1; j < defs->length; j++) {
        CaValue *later = fields(defs->items[j]);
        CaContainer *fs = (CaContainer *)later[FLAGS].as.obj;
        if (text(fs->items[0])[0] != '-' && later[NARGS].tag == CA_NONE_T)
          reserve++;
      }
      count = count > reserve ? count - reserve : 0;
    }
    roots[4] = scalar ? d[DEFAULT] : ca_container(rt, CA_LIST_T, 0, NULL);
    size_t consumed = 0;
    while (consumed < count && at < positionals->length && !rt->error) {
      roots[5] = convert(rt, parser, definition, positionals->items[at++]);
      if (!rt->error) {
        if (scalar)
          roots[4] = roots[5];
        else
          ca_append(rt, roots[4], roots[5]);
      }
      consumed++;
    }
    if (!rt->error && ((!consumed && (d[NARGS].tag == CA_NONE_T ||
                                      (d[NARGS].tag == CA_STR_T &&
                                       !strcmp(text(d[NARGS]), "+")))) ||
                       (d[NARGS].tag == CA_INT_T && consumed < count)))
      parse_error(rt, parser, "required positional argument is missing");
    if (!rt->error)
      ca_set_attribute(rt, roots[1], text(d[DEST]), roots[4]);
  }
  if (!rt->error && at < positionals->length)
    parse_error(rt, parser, "unrecognized positional arguments");
  free(seen);
  CaValue result = roots[1];
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : result;
}
CaValue ca_cli_call(CaRuntime *rt, int id, int n, int nkw, CaValue *args,
                    const char **keys) {
  if (id == N_SYS_EXIT) {
    if (n > 1 || nkw)
      goto bad;
    CaValue value = n ? args[0] : CA_NONE;
    CaValue error = ca_exception_new(rt, ca_type(CA_T_SYSTEMEXIT), 1, &value);
    if (!rt->error)
      ca_raise(rt, error, CA_NONE, 0);
    return CA_UNBOUND;
  }
  if (id == N_SUPPRESS) {
    if (nkw)
      goto bad;
    CaValue data = ca_container(rt, CA_TUPLE_T, (size_t)n, args);
    return rt->error ? CA_UNBOUND
                     : ca_native_object(rt, CA_F_SUPPRESS, data, NULL, 0);
  }
  if (id == N_NULLCONTEXT) {
    CaValue out[1], defaults[] = {CA_NONE};
    const char *names[] = {"enter_result"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 1, 0, names, defaults,
                             out))
      return CA_UNBOUND;
    return ca_native_object(rt, CA_F_NULLCONTEXT, out[0], NULL, 0);
  }
  if (id == N_SUPPRESS_ENTER || id == N_SUPPRESS_EXIT) {
    if (nkw || n != (id == N_SUPPRESS_ENTER ? 1 : 4) ||
        (!kind(args[0], CA_F_SUPPRESS) && !kind(args[0], CA_F_NULLCONTEXT)))
      goto bad;
    CaForeign *f = (CaForeign *)args[0].as.obj;
    if (id == N_SUPPRESS_ENTER)
      return f->kind == CA_F_NULLCONTEXT ? f->state : CA_NONE;
    if (args[2].tag == CA_NONE_T || f->kind == CA_F_NULLCONTEXT)
      return ca_bool(0);
    return ca_bool(ca_exception_match(rt, args[2], f->state));
  }
  if (id == N_PARSER) {
    const char *names[] = {"prog",
                           "usage",
                           "description",
                           "epilog",
                           "parents",
                           "formatter_class",
                           "prefix_chars",
                           "fromfile_prefix_chars",
                           "argument_default",
                           "conflict_handler",
                           "add_help",
                           "allow_abbrev",
                           "exit_on_error"};
    CaValue out[13], defaults[13];
    for (int i = 0; i < 13; i++)
      defaults[i] = CA_NONE;
    defaults[10] = defaults[11] = defaults[12] = ca_bool(1);
    if (!ca_native_arguments(rt, n, nkw, args, keys, 13, 0, names, defaults,
                             out))
      return CA_UNBOUND;
    for (int i = 0; i < 13; i++)
      if (i != 0 && i != 2 && i != 10 && i != 11 && out[i].tag != CA_NONE_T &&
          !(i == 12 && out[i].tag == CA_BOOL_T && out[i].as.i)) {
        ca_error(rt, "NotImplementedError",
                 "ArgumentParser option is not implemented");
        return CA_UNBOUND;
      }
    if (out[0].tag == CA_NONE_T) {
      CaContainer *argv = (CaContainer *)rt->arguments.as.obj;
      const char *name = argv->length ? text(argv->items[0]) : "program";
      const char *slash = strrchr(name, '/');
      name = slash ? slash + 1 : name;
      out[0] = ca_string(rt, name, strlen(name));
    }
    if (out[0].tag != CA_STR_T ||
        (out[2].tag != CA_STR_T && out[2].tag != CA_NONE_T))
      goto bad;
    CaModule *m = ca_standard_find(rt, "argparse");
    ca_module_start(rt, m);
    CaValue cls = ca_attribute(rt, ca_module_value(m), "Namespace", 0);
    CaValue values[] = {out[2],
                        out[0],
                        ca_container(rt, CA_LIST_T, 0, NULL),
                        cls,
                        ca_bool(ca_truth(rt, out[10])),
                        ca_bool(ca_truth(rt, out[11]))};
    CaValue data = ca_container(rt, CA_LIST_T, 6, values);
    return rt->error ? CA_UNBOUND
                     : ca_native_object(rt, CA_F_PARSER, data, NULL, 0);
  }
  if (id == N_ADD_ARGUMENT) {
    if (n < 2 || !kind(args[0], CA_F_PARSER))
      goto bad;
    CaValue values[DEF_COUNT] = {CA_UNBOUND, CA_NONE, ca_string(rt, "store", 5),
                                 CA_NONE,    CA_NONE, CA_NONE,
                                 ca_bool(0), CA_NONE, CA_NONE,
                                 CA_NONE};
    values[FLAGS] = ca_container(rt, CA_TUPLE_T, (size_t)n - 1, args + 1);
    for (int i = 1; i < n; i++)
      if (args[i].tag != CA_STR_T || !((CaString *)args[i].as.obj)->length)
        goto bad;
    for (int i = 0; i < nkw; i++) {
      const char *key = keys[i];
      int at = !strcmp(key, "dest")       ? DEST
               : !strcmp(key, "action")   ? ACTION
               : !strcmp(key, "nargs")    ? NARGS
               : !strcmp(key, "default")  ? DEFAULT
               : !strcmp(key, "type")     ? TYPE
               : !strcmp(key, "required") ? REQUIRED
               : !strcmp(key, "choices")  ? CHOICES
               : !strcmp(key, "help")     ? HELP
               : !strcmp(key, "metavar")  ? METAVAR
                                          : -1;
      if (at < 0) {
        ca_error(rt, "NotImplementedError",
                 "add_argument option is not implemented");
        return CA_UNBOUND;
      }
      values[at] = args[n + i];
    }
    if (values[ACTION].tag != CA_STR_T ||
        (strcmp(text(values[ACTION]), "store") &&
         strcmp(text(values[ACTION]), "store_true") &&
         strcmp(text(values[ACTION]), "store_false") &&
         strcmp(text(values[ACTION]), "count"))) {
      ca_error(rt, "NotImplementedError", "argparse action is not implemented");
      return CA_UNBOUND;
    }
    if (values[NARGS].tag != CA_NONE_T &&
        !(values[NARGS].tag == CA_INT_T && values[NARGS].as.i > 0) &&
        !(values[NARGS].tag == CA_STR_T &&
          ((CaString *)values[NARGS].as.obj)->length == 1 &&
          strchr("?*+", text(values[NARGS])[0])))
      goto bad;
    if (values[DEST].tag == CA_NONE_T) {
      const char *name = text(args[1]);
      for (int i = 1; i < n; i++)
        if (!strncmp(text(args[i]), "--", 2)) {
          name = text(args[i]);
          break;
        }
      while (*name == '-')
        name++;
      values[DEST] = ca_string(rt, name, strlen(name));
      CaString *s = (CaString *)values[DEST].as.obj;
      for (size_t i = 0; i < s->length; i++)
        if (s->data[i] == '-')
          s->data[i] = '_';
    }
    if (values[DEST].tag != CA_STR_T)
      goto bad;
    if (values[DEFAULT].tag == CA_NONE_T &&
        !strcmp(text(values[ACTION]), "store_true"))
      values[DEFAULT] = ca_bool(0);
    if (values[DEFAULT].tag == CA_NONE_T &&
        !strcmp(text(values[ACTION]), "store_false"))
      values[DEFAULT] = ca_bool(1);
    CaContainer *defs = (CaContainer *)fields(args[0])[DEFS].as.obj;
    for (int i = 1; i < n; i++)
      if (text(args[i])[0] == '-' &&
          matching(rt, args[0], text(args[i]), strlen(text(args[i]))) >= 0) {
        ca_error(rt, "ValueError", "conflicting option string");
        return CA_UNBOUND;
      }
    CaValue data = ca_container(rt, CA_LIST_T, DEF_COUNT, values);
    CaValue action = rt->error
                         ? CA_UNBOUND
                         : ca_native_object(rt, CA_F_ACTION, data, NULL, 0);
    if (!rt->error)
      ca_append(rt, fields(args[0])[DEFS], action);
    (void)defs;
    return rt->error ? CA_UNBOUND : action;
  }
  if (id == N_PARSE_ARGS || id == N_PARSE_INTERMIXED) {
    const char *names[] = {"self", "args", "namespace"};
    CaValue out[3], defaults[] = {CA_NONE, CA_NONE, CA_NONE};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 3, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    if (!kind(out[0], CA_F_PARSER))
      goto bad;
    return parse(rt, out[0], out[1], out[2]);
  }
  if (id == N_PARSER_ERROR) {
    if (n != 2 || nkw || !kind(args[0], CA_F_PARSER) || args[1].tag != CA_STR_T)
      goto bad;
    return parse_error(rt, args[0], text(args[1]));
  }
  if (id == N_PARSER_HELP) {
    if (n != 1 || nkw || !kind(args[0], CA_F_PARSER))
      goto bad;
    return help(rt, args[0], 0);
  }
bad:
  ca_error(rt, "TypeError", "invalid native CLI arguments");
  return CA_UNBOUND;
}
CaValue ca_cli_attribute(CaRuntime *rt, CaValue value, const char *name) {
  int id = 0;
  if (kind(value, CA_F_SUPPRESS) || kind(value, CA_F_NULLCONTEXT))
    id = !strcmp(name, "__enter__")  ? N_SUPPRESS_ENTER
         : !strcmp(name, "__exit__") ? N_SUPPRESS_EXIT
                                     : 0;
  if (kind(value, CA_F_PARSER))
    id = !strcmp(name, "add_argument")            ? N_ADD_ARGUMENT
         : !strcmp(name, "parse_args")            ? N_PARSE_ARGS
         : !strcmp(name, "parse_intermixed_args") ? N_PARSE_INTERMIXED
         : !strcmp(name, "error")                 ? N_PARSER_ERROR
         : !strcmp(name, "print_help")            ? N_PARSER_HELP
                                                  : 0;
  if (id)
    return ca_native_method(rt, value, id);
  ca_error(rt, "AttributeError", name);
  return CA_UNBOUND;
}
