#define _GNU_SOURCE
#include "standard.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
/* File state: encoding, original name/fd, normalized mode, newline, errors. */
CaValue ca_file_option(CaForeign *file, size_t at) {
  if (file->state.tag == CA_TUPLE_T &&
      ((CaContainer *)file->state.as.obj)->length == 5)
    return ca_capture(file->state, at);
  return at == 0 ? file->state : CA_NONE;
}
CaValue ca_file_decode(CaRuntime *rt, CaForeign *file, CaValue bytes) {
  CaValue roots[] = {bytes, ca_file_option(file, 0), ca_file_option(file, 4),
                     CA_UNBOUND};
  CaFrame f;
  ca_push(rt, &f, roots, 4);
  roots[3] = ca_text_call(rt, N_TEXT_DECODE, 3, 0, roots, NULL);
  if (!rt->error && ca_file_option(file, 3).tag == CA_NONE_T) {
    CaValue values[] = {roots[3], ca_string(rt, "\r\n", 2),
                        ca_string(rt, "\n", 1)};
    roots[3] = ca_text_call(rt, N_TEXT_REPLACE, 3, 0, values, NULL);
    values[0] = roots[3];
    values[1] = ca_string(rt, "\r", 1);
    if (!rt->error)
      roots[3] = ca_text_call(rt, N_TEXT_REPLACE, 3, 0, values, NULL);
  }
  CaValue result = roots[3];
  ca_pop(rt, &f);
  return rt->error ? CA_UNBOUND : result;
}
CaValue ca_builtin_open(CaRuntime *rt, int n, int nkw, CaValue *args,
                        const char **keys) {
  CaValue out[8], defaults[] = {CA_NONE,    ca_string(rt, "r", 1),
                                ca_int(-1), CA_NONE,
                                CA_NONE,    CA_NONE,
                                ca_bool(1), CA_NONE};
  const char *names[] = {"file",   "mode",    "buffering", "encoding",
                         "errors", "newline", "closefd",   "opener"};
  if (!ca_native_arguments(rt, n, nkw, args, keys, 8, 1, names, defaults, out))
    return CA_UNBOUND;
  const char *mode = ca_native_text(rt, out[1], 0);
  if (!mode)
    return CA_UNBOUND;
  int reading = 0, writing = 0, append = 0, exclusive = 0, binary = 0, text = 0,
      plus = 0;
  for (const char *p = mode; *p; p++) {
    int *counter = *p == 'r'   ? &reading
                   : *p == 'w' ? &writing
                   : *p == 'a' ? &append
                   : *p == 'x' ? &exclusive
                   : *p == 'b' ? &binary
                   : *p == 't' ? &text
                   : *p == '+' ? &plus
                               : NULL;
    if (!counter || ++*counter > 1) {
      ca_error(rt, "ValueError", "invalid file mode");
      return CA_UNBOUND;
    }
  }
  if (reading + writing + append + exclusive != 1 || binary + text > 1) {
    ca_error(
        rt, "ValueError",
        "must specify exactly one file operation and one text/binary mode");
    return CA_UNBOUND;
  }
  if (binary && (out[3].tag != CA_NONE_T || out[4].tag != CA_NONE_T ||
                 out[5].tag != CA_NONE_T)) {
    ca_error(rt, "ValueError",
             "binary mode does not take encoding/errors/newline");
    return CA_UNBOUND;
  }
  CaValue buffering = ca_builtin_index(rt, out[2]);
  if (rt->error)
    return CA_UNBOUND;
  if (buffering.tag != CA_INT_T || buffering.as.i > INT_MAX) {
    ca_error(rt, "OverflowError", "buffer size exceeds platform limits");
    return CA_UNBOUND;
  }
  if (!binary && !buffering.as.i) {
    ca_error(rt, "ValueError", "cannot have unbuffered text I/O");
    return CA_UNBOUND;
  }
  if (out[4].tag != CA_NONE_T) {
    const char *errors = ca_native_text(rt, out[4], 0);
    if (!errors)
      return CA_UNBOUND;
    if (strcmp(errors, "strict") && strcmp(errors, "ignore") &&
        strcmp(errors, "replace")) {
      ca_error(rt, "NotImplementedError",
               "native file error policy is not implemented");
      return CA_UNBOUND;
    }
  }
  int flags = plus ? O_RDWR : reading ? O_RDONLY : O_WRONLY;
  if (writing)
    flags |= O_CREAT | O_TRUNC;
  if (append)
    flags |= O_CREAT | O_APPEND;
  if (exclusive)
    flags |= O_CREAT | O_EXCL;
  int fd = -1, own = 0;
  CaValue roots[] = {out[0], out[3], out[4], out[5], CA_UNBOUND, CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 6);
  int closefd = ca_truth(rt, out[6]);
  if (rt->error)
    goto done;
  if (out[0].tag == CA_INT_T) {
    if (out[0].as.i < 0 || out[0].as.i > INT_MAX) {
      ca_error(rt, "ValueError", "invalid file descriptor");
      goto done;
    }
    fd = (int)out[0].as.i;
    if (!closefd) {
      fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
      own = 1;
      if (fd < 0) {
        ca_native_oserror(rt, errno, NULL);
        goto done;
      }
    }
    if (out[7].tag != CA_NONE_T) {
      ca_error(rt, "ValueError",
               "opener cannot be used with a file descriptor");
      goto done;
    }
  } else {
    if (!closefd) {
      ca_error(rt, "ValueError", "Cannot use closefd=False with a filename");
      goto done;
    }
    const char *filename = ca_native_text(rt, out[0], 1);
    if (!filename)
      goto done;
    roots[0] = ca_string(rt, filename, strlen(filename));
    if (out[7].tag != CA_NONE_T) {
      CaValue values[] = {out[0], ca_int(flags | O_CLOEXEC)};
      roots[4] = ca_call(rt, out[7], 2, 0, values, NULL);
      if (rt->error)
        goto done;
      roots[4] = ca_builtin_index(rt, roots[4]);
      if (rt->error)
        goto done;
      if (roots[4].tag != CA_INT_T || roots[4].as.i < 0 ||
          roots[4].as.i > INT_MAX) {
        ca_error(rt, "ValueError",
                 "opener returned an invalid file descriptor");
        goto done;
      }
      fd = (int)roots[4].as.i;
    } else
      fd = open(filename, flags | O_CLOEXEC, 0666);
    own = 1;
    if (fd < 0) {
      ca_native_oserror(rt, errno, filename);
      goto done;
    }
  }
  char fdmode[3] = {reading ? 'r' : append ? 'a' : 'w', plus ? '+' : 0, 0};
  CaValue values[] = {ca_int(fd), ca_string(rt, fdmode, strlen(fdmode)),
                      buffering, binary ? CA_NONE : out[3], out[5]};
  roots[5] = ca_io_call(rt, N_FDOPEN, 5, 0, values, NULL);
  if (!rt->error) {
    own = 0;
    CaForeign *file = (CaForeign *)roots[5].as.obj;
    file->auxiliary = binary;
    char actual[4] = {reading     ? 'r'
                      : append    ? 'a'
                      : exclusive ? 'x'
                                  : 'w',
                      0, 0, 0};
    size_t at = 1;
    if (binary)
      actual[at++] = 'b';
    if (plus)
      actual[at++] = '+';
    roots[4] = ca_string(rt, actual, at);
    CaValue metadata[] = {out[3], roots[0], roots[4], out[5], out[4]};
    file->state = ca_container(rt, CA_TUPLE_T, 5, metadata);
    if (!rt->error) {
      FILE *stream = file->handle;
      if (buffering.as.i == 0)
        setvbuf(stream, NULL, _IONBF, 0);
      else if (buffering.as.i == 1 && !binary)
        setvbuf(stream, NULL, _IOLBF, 0);
      else if (buffering.as.i > 1)
        setvbuf(stream, NULL, _IOFBF, (size_t)buffering.as.i);
    }
  }
done:
  if (own && fd >= 0 && rt->error)
    close(fd);
  CaValue result = roots[5];
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : result;
}
static int append_byte(char **buffer, size_t *length, size_t *capacity, int c) {
  if (*length == *capacity) {
    size_t next = *capacity ? *capacity * 2 : 128;
    if (next < *capacity)
      return 0;
    char *grown = realloc(*buffer, next);
    if (!grown)
      return 0;
    *buffer = grown;
    *capacity = next;
  }
  (*buffer)[(*length)++] = (char)c;
  return 1;
}
CaValue ca_file_read(CaRuntime *rt, CaValue value, long limit, int line) {
  CaForeign *file = (CaForeign *)value.as.obj;
  FILE *stream = file->handle;
  if (!stream) {
    ca_error(rt, "ValueError", "I/O operation on closed file");
    return CA_UNBOUND;
  }
  char *buffer = NULL;
  size_t length = 0, capacity = 0, count = 0;
  int binary = (file->auxiliary & 1),
      universal = ca_file_option(file, 3).tag == CA_NONE_T;
  while (limit < 0 || count < (size_t)limit) {
    int c = fgetc(stream);
    file->auxiliary |= 2;
    if (c == EOF)
      break;
    if (!binary && universal && c == '\r') {
      int next = fgetc(stream);
      if (next != EOF && next != '\n')
        ungetc(next, stream);
      c = '\n';
    }
    if (!append_byte(&buffer, &length, &capacity, c))
      goto memory;
    if (!binary && (unsigned)c >= 0xc0) {
      int more = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
      while (more--) {
        c = fgetc(stream);
        if (c == EOF)
          break;
        if (!append_byte(&buffer, &length, &capacity, c))
          goto memory;
      }
    }
    count++;
    if (line && c == '\n')
      break;
  }
  if (ferror(stream)) {
    free(buffer);
    return ca_native_oserror(rt, errno, NULL);
  }
  CaValue bytes = ca_bytes(rt, buffer, length);
  free(buffer);
  return rt->error || binary ? bytes : ca_file_decode(rt, file, bytes);
memory:
  free(buffer);
  ca_error(rt, "MemoryError", "file read allocation failed");
  return CA_UNBOUND;
}
CaValue ca_file_builtin(CaRuntime *rt, int id, int n, int nkw, CaValue *args,
                        const char **keys) {
  (void)keys;
  if (n < 1 || nkw || args[0].tag != CA_FOREIGN_T ||
      ((CaForeign *)args[0].as.obj)->kind != CA_F_FILE)
    goto bad;
  CaForeign *file = (CaForeign *)args[0].as.obj;
  FILE *stream = file->handle;
  if (!stream) {
    ca_error(rt, "ValueError", "I/O operation on closed file");
    return CA_UNBOUND;
  }
  if (id == 430) {
    if (n > 2)
      goto bad;
    CaValue size = n == 2 ? ca_builtin_index(rt, args[1]) : ca_int(-1);
    if (rt->error)
      return CA_UNBOUND;
    if (size.tag != CA_INT_T) {
      ca_error(rt, "OverflowError", "line size exceeds platform limit");
      return CA_UNBOUND;
    }
    return ca_file_read(rt, args[0], size.as.i, 1);
  }
  if (id == 431) {
    if (n > 2)
      goto bad;
    CaValue hint = n == 2 ? ca_builtin_index(rt, args[1]) : ca_int(-1);
    if (rt->error)
      return CA_UNBOUND;
    CaValue roots[] = {args[0], ca_container(rt, CA_LIST_T, 0, NULL),
                       CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 3);
    size_t total = 0;
    for (;;) {
      roots[2] = ca_file_read(rt, args[0], -1, 1);
      if (rt->error || !((CaString *)roots[2].as.obj)->length)
        break;
      CaValue length = ca_collection_builtin(rt, 3, 1, 0, &roots[2]);
      if (rt->error)
        break;
      total += (size_t)length.as.i;
      ca_append(rt, roots[1], roots[2]);
      if (rt->error ||
          (hint.tag == CA_INT_T && hint.as.i > 0 && total > (size_t)hint.as.i))
        break;
      ca_safepoint(rt);
    }
    CaValue result = roots[1];
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : result;
  }
  if (id == 432) {
    if (n != 2)
      goto bad;
    CaValue roots[] = {args[0], args[1], CA_UNBOUND, CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 4);
    roots[2] = ca_iter(rt, args[1]);
    while (!rt->error && ca_next(rt, roots[2], &roots[3]) > 0) {
      CaValue values[] = {args[0], roots[3]};
      ca_io_call(rt, N_FILE_WRITE, 2, 0, values, NULL);
      ca_safepoint(rt);
    }
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : CA_NONE;
  }
  if (id == 433) {
    if (n < 2 || n > 3)
      goto bad;
    CaValue offset = ca_builtin_index(rt, args[1]),
            whence = n == 3 ? ca_builtin_index(rt, args[2]) : ca_int(0);
    if (rt->error)
      return CA_UNBOUND;
    if (offset.tag != CA_INT_T || whence.tag != CA_INT_T) {
      ca_error(rt, "OverflowError", "file position exceeds platform size");
      return CA_UNBOUND;
    }
    if (whence.as.i < 0 || whence.as.i > 2)
      goto bad;
    if (!(file->auxiliary & 1) && whence.as.i && offset.as.i) {
      ca_error(rt, "OSError",
               "nonzero relative seeks in text files are unsupported");
      return CA_UNBOUND;
    }
    if (fseeko(stream, (off_t)offset.as.i, (int)whence.as.i))
      return ca_native_oserror(rt, errno, NULL);
    off_t pos = ftello(stream);
    return pos < 0 ? ca_native_oserror(rt, errno, NULL) : ca_int((int64_t)pos);
  }
  if (id == 434) {
    if (n != 1)
      goto bad;
    off_t pos = ftello(stream);
    return pos < 0 ? ca_native_oserror(rt, errno, NULL) : ca_int((int64_t)pos);
  }
bad:
  ca_error(rt, "TypeError", "invalid file method arguments");
  return CA_UNBOUND;
}

int ca_file_close(CaForeign *file) {
  FILE *stream = file->handle;
  if (!stream)
    return 0;
  int hold = -1;
  off_t position = -1;
  if (file->auxiliary & 2) {
    int fd = fileno(stream);
    position = lseek(fd, 0, SEEK_CUR);
    if (position >= 0)
      hold = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  }
  file->handle = NULL;
  int status = fclose(stream), error = errno;
  if (hold >= 0) {
    lseek(hold, position, SEEK_SET);
    close(hold);
  }
  errno = error;
  return status;
}
