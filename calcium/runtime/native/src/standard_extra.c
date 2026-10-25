#define _POSIX_C_SOURCE 200809L
#include "standard.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#ifndef CART_PY_MINOR
#define CART_PY_MINOR 14
#endif
void ca_extra_os(CaRuntime *rt, CaValue module) {
  const char *names[] = {"O_RDONLY", "O_WRONLY", "O_RDWR", "O_CREAT",
                         "O_TRUNC",  "O_APPEND", "O_EXCL", "O_CLOEXEC"};
  long values[] = {O_RDONLY, O_WRONLY, O_RDWR, O_CREAT,
                   O_TRUNC,  O_APPEND, O_EXCL, O_CLOEXEC};
  ca_set_attribute(rt, module, "open", ca_native_function(N_OS_OPEN));
  for (size_t i = 0; i < sizeof values / sizeof values[0]; i++)
    ca_set_attribute(rt, module, names[i], ca_int(values[i]));
}
CaValue ca_extra_call(CaRuntime *rt, int id, int n, int nkw, CaValue *args,
                      const char **keys) {
  if (id == N_PATH_GLOB)
    return ca_io_call(rt, id, n, nkw, args, keys);
  if (id == N_DICT_POP)
    return ca_dict_pop(rt, n, nkw, args, keys);
  if (id == N_OS_OPEN) {
    CaValue out[4], defaults[] = {CA_NONE, CA_NONE, ca_int(0777), CA_NONE};
    const char *names[] = {"path", "flags", "mode", "dir_fd"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 4, 2, names, defaults,
                             out))
      return CA_UNBOUND;
    const char *name = ca_native_text(rt, out[0], 1);
    if (!name)
      return CA_UNBOUND;
    if (out[1].tag != CA_INT_T || out[2].tag != CA_INT_T ||
        out[1].as.i > INT_MAX || out[1].as.i < INT_MIN)
      goto bad;
    if (out[3].tag != CA_NONE_T) {
      ca_error(rt, "NotImplementedError",
               "native os.open dir_fd is not implemented");
      return CA_UNBOUND;
    }
    int fd = open(name, (int)out[1].as.i | O_CLOEXEC, (mode_t)out[2].as.i);
    return fd < 0 ? ca_native_oserror(rt, errno, name) : ca_int(fd);
  }
  if (id == N_COMPARE_DIGEST) {
    if (n != 2 || nkw || args[0].tag != args[1].tag ||
        (args[0].tag != CA_STR_T && args[0].tag != CA_BYTES_T))
      goto bad;
    CaString *a = (CaString *)args[0].as.obj, *b = (CaString *)args[1].as.obj;
    if (args[0].tag == CA_STR_T) {
      for (size_t i = 0; i < a->length; i++)
        if ((unsigned char)a->data[i] >= 128)
          goto bad;
      for (size_t i = 0; i < b->length; i++)
        if ((unsigned char)b->data[i] >= 128)
          goto bad;
    }
    volatile size_t different = a->length ^ b->length;
    size_t count = a->length > b->length ? a->length : b->length;
    for (size_t i = 0; i < count; i++)
      different |= (size_t)((i < a->length ? (unsigned char)a->data[i] : 0) ^
                            (i < b->length ? (unsigned char)b->data[i] : 0));
    return ca_bool(!different);
  }
  if (id == N_PATH_UNLINK) {
    CaValue out[2], defaults[] = {CA_NONE, ca_bool(0)};
    const char *names[] = {"self", "missing_ok"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 2, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    const char *name = ca_native_text(rt, out[0], 1);
    if (!name)
      return CA_UNBOUND;
    if (unlink(name) < 0) {
      int error = errno;
      if (error != ENOENT || !ca_truth(rt, out[1]))
        return ca_native_oserror(rt, error, name);
    }
    return CA_NONE;
  }
  if (id == N_PATH_SUFFIX) {
    CaValue out[2], defaults[] = {CA_NONE, CA_NONE};
    const char *names[] = {"self", "suffix"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 2, 2, names, defaults,
                             out))
      return CA_UNBOUND;
    const char *name = ca_native_text(rt, out[0], 1),
               *suffix = ca_native_text(rt, out[1], 0);
    if (!name || !suffix)
      return CA_UNBOUND;
    if (strchr(suffix, '/') || (*suffix && *suffix != '.') ||
        (CART_PY_MINOR < 14 && !strcmp(suffix, "."))) {
      ca_error(rt, "ValueError", "Invalid suffix");
      return CA_UNBOUND;
    }
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (!*base || !strcmp(base, ".")) {
      ca_error(rt, "ValueError", "Path has an empty name");
      return CA_UNBOUND;
    }
    const char *dot = strrchr(base, '.');
    if (dot == base || (dot && !dot[1] && CART_PY_MINOR < 14))
      dot = NULL;
    size_t length = dot ? (size_t)(dot - name) : strlen(name),
           extra = strlen(suffix);
    if (extra > SIZE_MAX - length - 1) {
      ca_error(rt, "MemoryError", "path is too large");
      return CA_UNBOUND;
    }
    char *buffer = malloc(length + extra + 1);
    if (!buffer) {
      ca_error(rt, "MemoryError", "path allocation failed");
      return CA_UNBOUND;
    }
    memcpy(buffer, name, length);
    memcpy(buffer + length, suffix, extra + 1);
    CaValue result = ca_path_new(rt, buffer);
    free(buffer);
    return result;
  }
bad:
  ca_error(rt, "TypeError", "invalid native library arguments");
  return CA_UNBOUND;
}
