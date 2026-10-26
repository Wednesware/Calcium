#define _GNU_SOURCE
#include "dependencies.h"
CaValue ca_trace_capture(CaRuntime *rt) {
  CaFrame *frames[256];
  size_t count = 0;
  for (CaFrame *f = rt->roots; f && count < 256; f = f->previous)
    if (f->file && f->function)
      frames[count++] = f;
  if (!count)
    return ca_string(rt, "", 0);
  char *buffer = NULL;
  size_t size = 0;
  FILE *stream = open_memstream(&buffer, &size);
  if (!stream)
    return CA_NONE;
  while (count) {
    CaFrame *f = frames[--count];
    fprintf(stream, "  File \"%s\", line %d, in %s\n", f->file, f->line,
            f->function);
  }
  if (fclose(stream) != 0) {
    free(buffer);
    return CA_NONE;
  }
  CaValue value = ca_string(rt, buffer, size);
  free(buffer);
  return value;
}
static void render(CaRuntime *rt, FILE *stream, CaValue e, int only, int chain,
                   int depth) {
  if (depth > 32) {
    fputs("[native exception chain truncated]\n", stream);
    return;
  }
  if (e.tag == CA_NONE_T) {
    fputs("NoneType: None\n", stream);
    return;
  }
  if (e.tag != CA_EXCEPTION_T) {
    ca_error(rt, "TypeError", "expected an exception");
    return;
  }
  CaException *error = (CaException *)e.as.obj;
  if (chain && !only) {
    CaValue previous =
        error->cause.tag == CA_EXCEPTION_T ? error->cause : error->context;
    if (previous.tag == CA_EXCEPTION_T && previous.as.obj != e.as.obj) {
      render(rt, stream, previous, 0, chain, depth + 1);
      fputs(error->cause.tag == CA_EXCEPTION_T
                ? "\nThe above exception was the direct cause of the following "
                  "exception:\n\n"
                : "\nDuring handling of the above exception, another exception "
                  "occurred:\n\n",
            stream);
    }
  }
  if (!only && error->trace.tag == CA_STR_T &&
      ((CaString *)error->trace.as.obj)->length) {
    CaString *trace = (CaString *)error->trace.as.obj;
    fputs("Traceback (most recent call last):\n", stream);
    fwrite(trace->data, 1, trace->length, stream);
  }
  fputs(ca_type_name(error->type), stream);
  CaValue message = ca_stringify(rt, e, 0);
  if (!rt->error) {
    CaString *text = (CaString *)message.as.obj;
    if (text->length) {
      fputs(": ", stream);
      fwrite(text->data, 1, text->length, stream);
    }
    fputc('\n', stream);
  }
}
CaValue ca_trace_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                      const char **keys) {
  int current = id == D_TRACE_FORMAT_EXC || id == D_TRACE_PRINT_EXC,
      printing = id == D_TRACE_PRINT_EXC || id == D_TRACE_PRINT_EXCEPTION,
      only = id == D_TRACE_FORMAT_ONLY;
  CaValue exception = current ? rt->handled
                      : n     ? a[0]
                              : CA_UNBOUND,
          file = CA_NONE, limit = CA_NONE;
  int chain = 1;
  if (current) {
    if (n > 2)
      return ca_dep_error(rt, "TypeError", "too many traceback arguments");
    if (n)
      limit = a[0];
    if (n == 2) {
      if (printing)
        file = a[1];
      else
        chain = ca_truth(rt, a[1]);
    }
  } else if (n == 3)
    exception = a[1];
  else if (n != 1 && !(only && n == 2))
    return ca_dep_error(
        rt, "TypeError",
        "expected exception or legacy type/value/traceback arguments");
  else if (only && n == 2)
    exception = a[1];
  for (int i = 0; i < nkw; i++) {
    if (!strcmp(keys[i], "limit"))
      limit = a[n + i];
    else if (!strcmp(keys[i], "chain"))
      chain = ca_truth(rt, a[n + i]);
    else if (printing && !strcmp(keys[i], "file"))
      file = a[n + i];
    else
      return ca_dep_error(rt, "TypeError", "unsupported traceback option");
  }
  if (limit.tag != CA_NONE_T) {
    if (limit.tag == CA_INT_T && limit.as.i == 0)
      only = 1;
    else
      return ca_dep_error(rt, "NotImplementedError",
                          "native traceback supports limit=None or zero");
  }
  CaValue roots[] = {exception, file, CA_UNBOUND};
  CaFrame f;
  ca_push(rt, &f, roots, 3);
  char *buffer = NULL;
  size_t size = 0;
  FILE *stream = open_memstream(&buffer, &size);
  if (!stream) {
    ca_pop(rt, &f);
    return ca_dep_error(rt, "MemoryError",
                        "traceback formatting allocation failed");
  }
  render(rt, stream, exception, only, chain, 0);
  fclose(stream);
  if (!rt->error)
    roots[2] = ca_string(rt, buffer, size);
  free(buffer);
  if (!rt->error && printing) {
    if (file.tag == CA_NONE_T) {
      CaString *text = (CaString *)roots[2].as.obj;
      fwrite(text->data, 1, text->length, stderr);
    } else {
      CaValue write = ca_attribute(rt, file, "write", 0);
      if (!rt->error)
        ca_call(rt, write, 1, 0, &roots[2], NULL);
    }
    roots[2] = CA_NONE;
  } else if (!rt->error && !current)
    roots[2] = ca_container(rt, CA_LIST_T, 1, &roots[2]);
  CaValue result = roots[2];
  ca_pop(rt, &f);
  return rt->error ? CA_UNBOUND : result;
}
