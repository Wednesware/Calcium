#define _GNU_SOURCE
#include "standard.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#ifndef CART_PY_MINOR
#define CART_PY_MINOR 14
#endif
static char *duplicate(CaRuntime *rt, const char *s) {
  char *out = strdup(s);
  if (!out)
    ca_error(rt, "MemoryError", "path allocation failed");
  return out;
}
static char *join(CaRuntime *rt, const char *a, const char *b) {
  if (*b == '/')
    return duplicate(rt, b);
  size_t x = strlen(a), y = strlen(b);
  if (x > SIZE_MAX - y - 2) {
    ca_error(rt, "MemoryError", "path too large");
    return NULL;
  }
  char *out = malloc(x + y + 2);
  if (!out) {
    ca_error(rt, "MemoryError", "path allocation failed");
    return NULL;
  }
  memcpy(out, a, x);
  if (x && a[x - 1] != '/')
    out[x++] = '/';
  memcpy(out + x, b, y + 1);
  return out;
}
static CaValue path(CaRuntime *rt, const char *input) {
  size_t length = strlen(input);
  char *out = malloc(length + 3);
  if (!out) {
    ca_error(rt, "MemoryError", "path allocation failed");
    return CA_UNBOUND;
  }
  size_t at = 0, i = 0;
  if (input[0] == '/') {
    out[at++] = '/';
    i = 1;
    if (input[1] == '/' && input[2] != '/') {
      out[at++] = '/';
      i = 2;
    }
  }
  while (i < length) {
    while (input[i] == '/')
      i++;
    size_t start = i;
    while (input[i] && input[i] != '/')
      i++;
    size_t n = i - start;
    if (!n || (n == 1 && input[start] == '.'))
      continue;
    if (at && out[at - 1] != '/')
      out[at++] = '/';
    memcpy(out + at, input + start, n);
    at += n;
  }
  if (!at)
    out[at++] = '.';
  out[at] = 0;
  CaValue text = ca_string(rt, out, at);
  free(out);
  return rt->error ? CA_UNBOUND
                   : ca_native_object(rt, CA_F_PATH, text, NULL, 0);
}
CaValue ca_path_new(CaRuntime *rt, const char *input) {
  return path(rt, input);
}
static CaValue path_string(CaValue value) {
  return ((CaForeign *)value.as.obj)->state;
}
static CaValue read_file(CaRuntime *rt, const char *name) {
  FILE *stream = fopen(name, "rb");
  if (!stream)
    return ca_native_oserror(rt, errno, name);
  char *buffer = NULL;
  size_t used = 0, capacity = 0;
  int error = 0;
  for (;;) {
    if (capacity - used < 65536) {
      if (capacity > SIZE_MAX - 65536) {
        error = ENOMEM;
        break;
      }
      capacity += 65536;
      char *next = realloc(buffer, capacity);
      if (!next) {
        error = ENOMEM;
        break;
      }
      buffer = next;
    }
    size_t got = fread(buffer + used, 1, capacity - used, stream);
    used += got;
    if (!got) {
      if (ferror(stream))
        error = errno ? errno : EIO;
      break;
    }
  }
  if (fclose(stream) && !error)
    error = errno;
  CaValue result =
      error ? ca_native_oserror(rt, error, name) : ca_bytes(rt, buffer, used);
  free(buffer);
  return result;
}
static int mkdir_path(const char *name, mode_t mode, int parents,
                      int exist_ok) {
  if (!mkdir(name, mode))
    return 0;
  int error = errno;
  if (error == EEXIST && exist_ok) {
    struct stat s;
    if (!stat(name, &s) && S_ISDIR(s.st_mode))
      return 0;
    return error;
  }
  if (error == ENOENT && parents) {
    char *copy = strdup(name);
    if (!copy)
      return ENOMEM;
    char *slash = strrchr(copy, '/');
    if (slash && slash != copy) {
      *slash = 0;
      error = mkdir_path(copy, 0777, 1, 1);
      if (!error && mkdir(name, mode))
        error = errno;
      free(copy);
      return error;
    }
    free(copy);
  }
  return error;
}
static char *absolute(CaRuntime *rt, const char *name) {
  if (*name == '/')
    return duplicate(rt, name);
  char *cwd = getcwd(NULL, 0);
  if (!cwd) {
    ca_native_oserror(rt, errno, name);
    return NULL;
  }
  char *out = join(rt, cwd, name);
  free(cwd);
  return out;
}
static char *resolve_path(CaRuntime *rt, const char *name, int strict) {
  char *result = realpath(name, NULL);
  if (result)
    return result;
  int error = errno;
  if (strict || (error != ENOENT && error != ENOTDIR)) {
    ca_native_oserror(rt, error, name);
    return NULL;
  }
  char *full = absolute(rt, name);
  if (!full)
    return NULL;
  char *resolved = calloc(1, strlen(full) + 2);
  if (!resolved) {
    free(full);
    ca_error(rt, "MemoryError", "path resolution allocation failed");
    return NULL;
  }
  strcpy(resolved, "/");
  char *save = NULL;
  for (char *part = strtok_r(full, "/", &save); part;
       part = strtok_r(NULL, "/", &save)) {
    if (!strcmp(part, "."))
      continue;
    if (!strcmp(part, "..")) {
      size_t n = strlen(resolved);
      while (n > 1 && resolved[n - 1] != '/')
        n--;
      if (n > 1)
        n--;
      resolved[n] = 0;
      continue;
    }
    char *next = join(rt, resolved, part);
    if (!next) {
      free(full);
      free(resolved);
      return NULL;
    }
    char *link = realpath(next, NULL);
    free(resolved);
    resolved = link ? link : next;
    if (link)
      free(next);
  }
  free(full);
  return resolved;
}
static int remove_directory_fd(int parent, const char *name) {
  int fd =
      openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return errno;
  DIR *directory = fdopendir(fd);
  if (!directory) {
    int e = errno;
    close(fd);
    return e;
  }
  int error = 0;
  struct dirent *entry;
  while (!error && (entry = readdir(directory))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    struct stat st;
    if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
      error = errno;
      break;
    }
    if (S_ISDIR(st.st_mode))
      error = remove_directory_fd(fd, entry->d_name);
    else if (unlinkat(fd, entry->d_name, 0))
      error = errno;
  }
  if (closedir(directory) && !error)
    error = errno;
  if (!error && unlinkat(parent, name, AT_REMOVEDIR))
    error = errno;
  return error;
}
static int copy_tree(const char *source, const char *destination,
                     int dirs_exist_ok, unsigned depth) {
  if (depth > 256)
    return ELOOP;
  struct stat st;
  if (stat(source, &st))
    return errno;
  if (S_ISDIR(st.st_mode)) {
    int error = mkdir_path(destination, 0700, 0, dirs_exist_ok);
    if (error)
      return error;
    DIR *dir = opendir(source);
    if (!dir)
      return errno;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        continue;
      size_t a = strlen(source) + strlen(entry->d_name) + 2,
             b = strlen(destination) + strlen(entry->d_name) + 2;
      char *s = malloc(a), *d = malloc(b);
      if (!s || !d) {
        free(s);
        free(d);
        error = ENOMEM;
        break;
      }
      snprintf(s, a, "%s/%s", source, entry->d_name);
      snprintf(d, b, "%s/%s", destination, entry->d_name);
      error = copy_tree(s, d, dirs_exist_ok, depth + 1);
      free(s);
      free(d);
      if (error)
        break;
    }
    closedir(dir);
    if (error)
      return error;
  } else {
    if (!S_ISREG(st.st_mode))
      return EINVAL;
    FILE *in = fopen(source, "rb");
    if (!in)
      return errno;
    FILE *out = fopen(destination, "wb");
    if (!out) {
      int e = errno;
      fclose(in);
      return e;
    }
    char buffer[65536];
    size_t n;
    int error = 0;
    while ((n = fread(buffer, 1, sizeof buffer, in)))
      if (fwrite(buffer, 1, n, out) != n) {
        error = errno ? errno : EIO;
        break;
      }
    if (ferror(in) && !error)
      error = errno ? errno : EIO;
    fclose(in);
    if (fclose(out) && !error)
      error = errno;
    if (error)
      return error;
  }
  if (chmod(destination, st.st_mode & 07777))
    return errno;
  struct timespec times[] = {st.st_atim, st.st_mtim};
  if (utimensat(AT_FDCWD, destination, times, 0))
    return errno;
  return 0;
}
typedef struct WalkLevel {
  DIR *directory;
  char *path;
  struct WalkLevel *previous;
} WalkLevel;
typedef struct {
  WalkLevel *top;
  char *pattern;
  int recursive;
} Walk;
static int walk_push(Walk *walk, const char *name) {
  DIR *dir = opendir(name);
  if (!dir)
    return errno;
  WalkLevel *level = calloc(1, sizeof *level);
  if (!level) {
    closedir(dir);
    return ENOMEM;
  }
  level->directory = dir;
  level->path = strdup(name);
  if (!level->path) {
    closedir(dir);
    free(level);
    return ENOMEM;
  }
  level->previous = walk->top;
  walk->top = level;
  return 0;
}
static void walk_pop(Walk *walk) {
  WalkLevel *top = walk->top;
  if (!top)
    return;
  walk->top = top->previous;
  closedir(top->directory);
  free(top->path);
  free(top);
}
int ca_io_next(CaRuntime *rt, CaValue value, CaValue *out) {
  Walk *walk = ((CaForeign *)value.as.obj)->handle;
  if (!walk) {
    *out = CA_UNBOUND;
    return 0;
  }
  while (walk->top) {
    WalkLevel *top = walk->top;
    errno = 0;
    struct dirent *entry = readdir(top->directory);
    if (!entry) {
      int error = errno;
      walk_pop(walk);
      if (error) {
        ca_native_oserror(rt, error, NULL);
        return -1;
      }
      continue;
    }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    char *name = join(rt, top->path, entry->d_name);
    if (!name)
      return -1;
    struct stat st;
    int matches = fnmatch(walk->pattern, entry->d_name, 0) == 0;
    if (walk->recursive && !lstat(name, &st) && S_ISDIR(st.st_mode)) {
      int error = walk_push(walk, name);
      if (error && error != EACCES && error != ENOENT) {
        free(name);
        ca_native_oserror(rt, error, NULL);
        return -1;
      }
    }
    if (matches) {
      *out = path(rt, name);
      free(name);
      return rt->error ? -1 : 1;
    }
    free(name);
  }
  *out = CA_UNBOUND;
  return 0;
}
void ca_io_finalize(CaForeign *foreign) {
  if (foreign->kind == CA_F_FILE && foreign->handle) {
    ca_file_close(foreign);
  }
  if (foreign->kind == CA_F_DIRECTORY_ITER && foreign->handle) {
    Walk *w = foreign->handle;
    while (w->top)
      walk_pop(w);
    free(w->pattern);
    free(w);
    foreign->handle = NULL;
  }
}
CaValue ca_path_attribute(CaRuntime *rt, CaValue value, const char *attribute) {
  if (!strcmp(attribute, "glob"))
    return ca_native_method(rt, value, N_PATH_GLOB);
  if (!strcmp(attribute, "unlink"))
    return ca_native_method(rt, value, N_PATH_UNLINK);
  if (!strcmp(attribute, "with_suffix"))
    return ca_native_method(rt, value, N_PATH_SUFFIX);
  const char *name = ca_native_text(rt, value, 1);
  if (!name)
    return CA_UNBOUND;
  size_t length = strlen(name);
  const char *last = strrchr(name, '/');
  const char *base = last                 ? last + 1
                     : !strcmp(name, ".") ? name + length
                                          : name;
  if (!strcmp(attribute, "name"))
    return ca_string(rt, base, strlen(base));
  if (!strcmp(attribute, "parent")) {
    if (!*base || !last)
      return path(rt, last ? name : ".");
    size_t n = (size_t)(last - name);
    if (!n)
      n = 1;
    char *copy = strndup(name, n);
    if (!copy) {
      ca_error(rt, "MemoryError", "path allocation failed");
      return CA_UNBOUND;
    }
    CaValue out = path(rt, copy);
    free(copy);
    return out;
  }
  if (!strcmp(attribute, "suffix") || !strcmp(attribute, "stem")) {
    const char *dot = strrchr(base, '.');
    int suffix = dot && dot != base && (CART_PY_MINOR >= 14 || dot[1]);
    if (!strcmp(attribute, "suffix"))
      return ca_string(rt, suffix ? dot : "", suffix ? strlen(dot) : 0);
    return ca_string(rt, base, suffix ? (size_t)(dot - base) : strlen(base));
  }
  if (!strcmp(attribute, "parts")) {
    CaValue list = ca_container(rt, CA_LIST_T, 0, NULL);
    char *copy = duplicate(rt, name);
    if (!copy)
      return CA_UNBOUND;
    if (*name == '/')
      ca_append(rt, list, ca_string(rt, name, !strncmp(name, "//", 2) ? 2 : 1));
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part && !rt->error;
         part = strtok_r(NULL, "/", &save))
      if (strcmp(part, "."))
        ca_append(rt, list, ca_string(rt, part, strlen(part)));
    free(copy);
    return rt->error ? CA_UNBOUND : ca_collection_builtin(rt, 5, 1, 0, &list);
  }
  const char *names[] = {
      "resolve",    "expanduser",  "mkdir",    "read_text", "write_text",
      "read_bytes", "write_bytes", "is_file",  "is_dir",    "exists",
      "stat",       "relative_to", "as_posix", "rglob",     "iterdir"};
  const int ids[] = {N_PATH_RESOLVE,       N_PATH_EXPANDUSER, N_PATH_MKDIR,
                     N_PATH_READTEXT,      N_PATH_WRITETEXT,  N_PATH_READBYTES,
                     N_PATH_WRITEBYTES,    N_PATH_IS_FILE,    N_PATH_IS_DIR,
                     N_PATH_EXISTS_METHOD, N_PATH_STAT,       N_PATH_RELATIVE,
                     N_PATH_AS_POSIX,      N_PATH_RGLOB,      N_PATH_ITERDIR};
  for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++)
    if (!strcmp(attribute, names[i]))
      return ca_native_method(rt, value, ids[i]);
  ca_error(rt, "AttributeError", attribute);
  return CA_UNBOUND;
}
#define BIND(total, required, ...)                                             \
  const char *names[] = {__VA_ARGS__};                                         \
  if (!ca_native_arguments(rt, n, nkw, args, keys, total, required, names,     \
                           defaults, out))                                     \
  return CA_UNBOUND
CaValue ca_io_call(CaRuntime *rt, int kind, int n, int nkw, CaValue *args,
                   const char **keys) {
  int glob_only = kind == N_PATH_GLOB;
  if (glob_only)
    kind = N_PATH_RGLOB;
  CaValue out[8], defaults[8] = {CA_NONE, CA_NONE, CA_NONE, CA_NONE,
                                 CA_NONE, CA_NONE, CA_NONE, CA_NONE};
  if (kind == CA_T_PATH) {
    if (nkw) {
      ca_error(rt, "TypeError", "Path does not accept keyword arguments");
      return CA_UNBOUND;
    }
    char *text = duplicate(rt, "");
    if (!text)
      return CA_UNBOUND;
    for (int i = 0; i < n; i++) {
      const char *part = ca_native_text(rt, args[i], 1);
      if (!part) {
        free(text);
        return CA_UNBOUND;
      }
      char *next = join(rt, text, part);
      free(text);
      if (!next)
        return CA_UNBOUND;
      text = next;
    }
    CaValue result = path(rt, text);
    free(text);
    return result;
  }
  if (kind == N_GETCWD) {
    BIND(0, 0, "");
    char *cwd = getcwd(NULL, 0);
    if (!cwd)
      return ca_native_oserror(rt, errno, NULL);
    CaValue result = ca_string(rt, cwd, strlen(cwd));
    free(cwd);
    return result;
  }
  if (kind == N_GETENV) {
    BIND(2, 1, "key", "default");
    const char *key = ca_native_text(rt, out[0], 0);
    if (!key)
      return CA_UNBOUND;
    const char *found = getenv(key);
    return found ? ca_string(rt, found, strlen(found)) : out[1];
  }
  if (kind == N_REPLACE) {
    BIND(2, 2, "src", "dst");
    const char *src = ca_native_text(rt, out[0], 1),
               *dst = ca_native_text(rt, out[1], 1);
    if (rt->error)
      return CA_UNBOUND;
    if (rename(src, dst))
      return ca_native_oserror(rt, errno, src);
    return CA_NONE;
  }
  if (kind == N_UNLINK) {
    BIND(1, 1, "path");
    const char *name = ca_native_text(rt, out[0], 1);
    if (!name)
      return CA_UNBOUND;
    if (unlink(name))
      return ca_native_oserror(rt, errno, name);
    return CA_NONE;
  }
  if (kind == N_FSYNC) {
    BIND(1, 1, "fd");
    if (out[0].tag != CA_INT_T) {
      ca_error(rt, "TypeError", "file descriptor must be an integer");
      return CA_UNBOUND;
    }
    if (fsync((int)out[0].as.i))
      return ca_native_oserror(rt, errno, NULL);
    return CA_NONE;
  }
  if (kind == N_FDOPEN) {
    defaults[1] = ca_string(rt, "r", 1);
    defaults[2] = ca_int(-1);
    BIND(5, 1, "fd", "mode", "buffering", "encoding", "newline");
    if (out[0].tag != CA_INT_T) {
      ca_error(rt, "TypeError", "file descriptor must be an integer");
      return CA_UNBOUND;
    }
    const char *mode = ca_native_text(rt, out[1], 0);
    if (!mode)
      return CA_UNBOUND;
    if (strcmp(mode, "r") && strcmp(mode, "w") && strcmp(mode, "a") &&
        strcmp(mode, "rb") && strcmp(mode, "wb") && strcmp(mode, "ab") &&
        strcmp(mode, "r+") && strcmp(mode, "w+") && strcmp(mode, "a+")) {
      ca_error(rt, "ValueError", "native file mode is not implemented");
      return CA_UNBOUND;
    }
    if (out[4].tag != CA_NONE_T &&
        (out[4].tag != CA_STR_T || ((CaString *)out[4].as.obj)->length)) {
      ca_error(rt, "NotImplementedError",
               "native newline translation mode is not implemented");
      return CA_UNBOUND;
    }
    if (strchr(mode, 'b') && out[3].tag != CA_NONE_T) {
      ca_error(rt, "ValueError",
               "binary mode does not take an encoding argument");
      return CA_UNBOUND;
    }
    if (out[3].tag != CA_NONE_T) {
      const char *encoding = ca_native_text(rt, out[3], 0);
      if (!encoding)
        return CA_UNBOUND;
      if (strcmp(encoding, "utf-8") && strcmp(encoding, "utf8") &&
          strcmp(encoding, "ascii")) {
        ca_error(rt, "NotImplementedError",
                 "native fdopen supports UTF-8 and ASCII encoding");
        return CA_UNBOUND;
      }
    }
    FILE *f = fdopen((int)out[0].as.i, mode);
    if (!f)
      return ca_native_oserror(rt, errno, NULL);
    CaValue metadata =
        ca_container(rt, CA_TUPLE_T, 5,
                     (CaValue[]){out[3], out[0], out[1], out[4], CA_NONE});
    if (rt->error) {
      fclose(f);
      return CA_UNBOUND;
    }
    CaValue result =
        ca_native_object(rt, CA_F_FILE, metadata, f, strchr(mode, 'b') != NULL);
    if (rt->error)
      fclose(f);
    return result;
  }
  if (kind >= N_FILE_READ && kind <= N_FILE_EXIT) {
    int total = kind == N_FILE_READ    ? 2
                : kind == N_FILE_WRITE ? 2
                : kind == N_FILE_EXIT  ? 4
                                       : 1;
    defaults[1] = ca_int(-1);
    const char *names[] = {"self", "value", "exception", "traceback"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, total,
                             kind == N_FILE_WRITE ? 2 : 1, names, defaults,
                             out))
      return CA_UNBOUND;
    if (out[0].tag != CA_FOREIGN_T ||
        ((CaForeign *)out[0].as.obj)->kind != CA_F_FILE) {
      ca_error(rt, "TypeError", "file method requires a file");
      return CA_UNBOUND;
    }
    CaForeign *file = (CaForeign *)out[0].as.obj;
    FILE *f = file->handle;
    if (kind == N_FILE_CLOSE || kind == N_FILE_EXIT) {
      if (f) {
        if (ca_file_close(file))
          return ca_native_oserror(rt, errno, NULL);
      }
      return CA_NONE;
    }
    if (!f) {
      ca_error(rt, "ValueError", "I/O operation on closed file");
      return CA_UNBOUND;
    }
    if (kind == N_FILE_ENTER)
      return out[0];
    if (kind == N_FILE_FILENO)
      return ca_int(fileno(f));
    if (kind == N_FILE_FLUSH) {
      if (!(file->auxiliary & 2) && fflush(f))
        return ca_native_oserror(rt, errno, NULL);
      return CA_NONE;
    }
    if (kind == N_FILE_WRITE) {
      if (out[1].tag != ((file->auxiliary & 1) ? CA_BYTES_T : CA_STR_T)) {
        ca_error(rt, "TypeError", "file write argument has wrong type");
        return CA_UNBOUND;
      }
      CaString *s = (CaString *)out[1].as.obj;
      CaValue encoded = out[1];
      if (!(file->auxiliary & 1)) {
        CaValue arguments[] = {out[1], ca_file_option(file, 0),
                               ca_file_option(file, 4)};
        encoded = ca_text_call(rt, N_TEXT_ENCODE, 3, 0, arguments, NULL);
        if (rt->error)
          return CA_UNBOUND;
      }
      CaString *bytes = (CaString *)encoded.as.obj;
      file->auxiliary &= ~2L;
      if (fwrite(bytes->data, 1, bytes->length, f) != bytes->length)
        return ca_native_oserror(rt, errno, NULL);
      size_t count = 0;
      if (file->auxiliary & 1)
        count = s->length;
      else
        for (size_t i = 0; i < s->length; i++)
          if (((unsigned char)s->data[i] & 0xc0) != 0x80)
            count++;
      return ca_int((int64_t)count);
    }
    if (out[1].tag != CA_INT_T) {
      ca_error(rt, "TypeError", "read size must be an integer");
      return CA_UNBOUND;
    }
    if (!(file->auxiliary & 1) && out[1].as.i >= 0)
      return ca_file_read(rt, out[0], out[1].as.i, 0);
    size_t limit = out[1].as.i < 0 ? SIZE_MAX : (size_t)out[1].as.i;
    char *buffer = NULL;
    size_t used = 0, capacity = 0;
    while (used < limit) {
      size_t chunk = limit - used < 65536 ? limit - used : 65536;
      if (capacity - used < chunk) {
        if (chunk > SIZE_MAX - used) {
          free(buffer);
          ca_error(rt, "MemoryError", "read buffer too large");
          return CA_UNBOUND;
        }
        char *next = realloc(buffer, used + chunk);
        if (!next) {
          free(buffer);
          ca_error(rt, "MemoryError", "read buffer allocation failed");
          return CA_UNBOUND;
        }
        buffer = next;
        capacity = used + chunk;
      }
      size_t got = fread(buffer + used, 1, chunk, f);
      file->auxiliary |= 2;
      used += got;
      if (got < chunk) {
        if (ferror(f)) {
          free(buffer);
          return ca_native_oserror(rt, errno ? errno : EIO, NULL);
        }
        break;
      }
    }
    CaValue bytes = ca_bytes(rt, buffer, used);
    free(buffer);
    if (rt->error || (file->auxiliary & 1))
      return bytes;
    return ca_file_decode(rt, file, bytes);
  }
  if (kind == N_GETTEMPDIR) {
    BIND(0, 0, "");
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
      dir = "/tmp";
    return ca_string(rt, dir, strlen(dir));
  }
  if (kind == N_MKSTEMP) {
    defaults[0] = ca_string(rt, "", 0);
    defaults[1] = ca_string(rt, "tmp", 3);
    defaults[3] = ca_bool(0);
    BIND(4, 0, "suffix", "prefix", "dir", "text");
    const char *suffix = ca_native_text(rt, out[0], 0),
               *prefix = ca_native_text(rt, out[1], 0),
               *directory = out[2].tag == CA_NONE_T
                                ? getenv("TMPDIR")
                                : ca_native_text(rt, out[2], 1);
    if (rt->error)
      return CA_UNBOUND;
    if (!directory || !*directory)
      directory = "/tmp";
    size_t size = strlen(directory) + strlen(prefix) + strlen(suffix) + 10;
    char *name = malloc(size);
    if (!name) {
      ca_error(rt, "MemoryError", "temporary path allocation failed");
      return CA_UNBOUND;
    }
    snprintf(name, size, "%s/%sXXXXXX%s", directory, prefix, suffix);
    int fd = mkstemps(name, (int)strlen(suffix));
    if (fd < 0) {
      CaValue result = ca_native_oserror(rt, errno, name);
      free(name);
      return result;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    CaValue values[] = {ca_int(fd), ca_string(rt, name, strlen(name))};
    CaValue result = ca_container(rt, CA_TUPLE_T, 2, values);
    if (rt->error) {
      close(fd);
      unlink(name);
    }
    free(name);
    return result;
  }
  if (kind == N_COPYTREE) {
    defaults[2] = ca_bool(0);
    defaults[3] = ca_bool(0);
    BIND(4, 2, "src", "dst", "symlinks", "dirs_exist_ok");
    if (ca_truth(rt, out[2])) {
      ca_error(rt, "NotImplementedError",
               "copytree symlinks=True is not implemented");
      return CA_UNBOUND;
    }
    const char *src = ca_native_text(rt, out[0], 1),
               *dst = ca_native_text(rt, out[1], 1);
    if (rt->error)
      return CA_UNBOUND;
    int error = copy_tree(src, dst, ca_truth(rt, out[3]), 0);
    if (error)
      return ca_native_oserror(rt, error, src);
    return out[1];
  }
  if (kind == N_RMTREE) {
    defaults[1] = ca_bool(0);
    BIND(2, 1, "path", "ignore_errors");
    const char *name = ca_native_text(rt, out[0], 1);
    if (!name)
      return CA_UNBOUND;
    int error = remove_directory_fd(AT_FDCWD, name);
    if (error && !ca_truth(rt, out[1]))
      return ca_native_oserror(rt, error, name);
    return CA_NONE;
  }
  if (kind >= N_PATH_EXISTS && kind <= N_PATH_BASENAME) {
    if (kind == N_PATH_JOIN) {
      if (!n || nkw) {
        ca_error(rt, "TypeError", "join expects positional path arguments");
        return CA_UNBOUND;
      }
      char *joined = duplicate(rt, "");
      if (!joined)
        return CA_UNBOUND;
      for (int i = 0; i < n; i++) {
        const char *part = ca_native_text(rt, args[i], 1);
        if (!part) {
          free(joined);
          return CA_UNBOUND;
        }
        char *next = join(rt, joined, part);
        free(joined);
        if (!next)
          return CA_UNBOUND;
        joined = next;
      }
      CaValue result = ca_string(rt, joined, strlen(joined));
      free(joined);
      return result;
    }
    BIND(1, 1, "path");
    const char *name = ca_native_text(rt, out[0], 1);
    if (!name)
      return CA_UNBOUND;
    if (kind <= N_PATH_ISDIR) {
      struct stat st;
      int ok = stat(name, &st) == 0;
      return ca_bool(ok && (kind == N_PATH_EXISTS ||
                            (kind == N_PATH_ISFILE ? S_ISREG(st.st_mode)
                                                   : S_ISDIR(st.st_mode))));
    }
    if (kind == N_PATH_ABSPATH) {
      char *input = absolute(rt, name);
      if (!input)
        return CA_UNBOUND;
      size_t length = strlen(input),
             at = input[0] == '/' && input[1] == '/' && input[2] != '/' ? 2 : 1;
      char *full = calloc(length + 2, 1);
      if (!full) {
        free(input);
        ca_error(rt, "MemoryError", "path allocation failed");
        return CA_UNBOUND;
      }
      memcpy(full, input, at);
      size_t root = at;
      for (size_t i = at; i < length;) {
        while (input[i] == '/')
          i++;
        size_t start = i;
        while (input[i] && input[i] != '/')
          i++;
        size_t n = i - start;
        if (!n || (n == 1 && input[start] == '.'))
          continue;
        if (n == 2 && input[start] == '.' && input[start + 1] == '.') {
          while (at > root && full[at - 1] != '/')
            at--;
          if (at > root)
            at--;
        } else {
          if (at > root)
            full[at++] = '/';
          memcpy(full + at, input + start, n);
          at += n;
        }
      }
      full[at] = 0;
      free(input);
      if (!full)
        return CA_UNBOUND;
      CaValue result = ca_string(rt, full, strlen(full));
      free(full);
      return result;
    }
    const char *slash = strrchr(name, '/');
    if (kind == N_PATH_BASENAME)
      return ca_string(rt, slash ? slash + 1 : name,
                       strlen(slash ? slash + 1 : name));
    size_t length = slash ? (size_t)(slash - name) : 0;
    while (length > 1 && name[length - 1] == '/')
      length--;
    if (slash && !length)
      length = 1;
    return ca_string(rt, name, length);
  }
  if (kind == N_PATH_HOME) {
    BIND(0, 0, "");
    const char *home = getenv("HOME");
    if (!home) {
      struct passwd *entry = getpwuid(getuid());
      home = entry ? entry->pw_dir : NULL;
    }
    if (!home) {
      ca_error(rt, "RuntimeError", "could not determine home directory");
      return CA_UNBOUND;
    }
    return path(rt, home);
  }
  if (kind >= N_PATH_RESOLVE && kind <= N_PATH_ITERDIR) {
    int total = kind == N_PATH_MKDIR       ? 4
                : kind == N_PATH_READTEXT  ? 4
                : kind == N_PATH_WRITETEXT ? 5
                : kind == N_PATH_WRITEBYTES || kind == N_PATH_RELATIVE ||
                        kind == N_PATH_RGLOB || kind == N_PATH_RESOLVE
                    ? 2
                    : 1;
    const char *names[5] = {"self", "value", "encoding", "errors", "newline"};
    defaults[1] = CA_NONE;
    defaults[2] = CA_NONE;
    defaults[3] = CA_NONE;
    defaults[4] = CA_NONE;
    if (kind == N_PATH_MKDIR) {
      names[1] = "mode";
      names[2] = "parents";
      names[3] = "exist_ok";
      defaults[1] = ca_int(0777);
      defaults[2] = ca_bool(0);
      defaults[3] = ca_bool(0);
    }
    if (kind == N_PATH_RESOLVE) {
      names[1] = "strict";
      defaults[1] = ca_bool(0);
    }
    if (kind == N_PATH_READTEXT) {
      names[1] = "encoding";
      names[2] = "errors";
      names[3] = "newline";
    }
    if (kind == N_PATH_WRITETEXT) {
      names[1] = "data";
    }
    if (kind == N_PATH_RGLOB)
      names[1] = "pattern";
    if (!ca_native_arguments(
            rt, n, nkw, args, keys, total,
            kind == N_PATH_WRITEBYTES || kind == N_PATH_RELATIVE ||
                    kind == N_PATH_RGLOB || kind == N_PATH_WRITETEXT
                ? 2
                : 1,
            names, defaults, out))
      return CA_UNBOUND;
    const char *name = ca_native_text(rt, out[0], 1);
    if (!name)
      return CA_UNBOUND;
    if (kind == N_PATH_AS_POSIX)
      return path_string(out[0]);
    if (kind == N_PATH_RESOLVE) {
      char *resolved = resolve_path(rt, name, ca_truth(rt, out[1]));
      if (!resolved)
        return CA_UNBOUND;
      CaValue result = path(rt, resolved);
      free(resolved);
      return result;
    }
    if (kind == N_PATH_EXPANDUSER) {
      if (name[0] != '~')
        return out[0];
      const char *slash = strchr(name, '/');
      size_t user = slash ? (size_t)(slash - name - 1) : strlen(name + 1);
      const char *home = NULL;
      char *username = NULL;
      if (!user)
        home = getenv("HOME");
      else {
        username = strndup(name + 1, user);
        if (!username) {
          ca_error(rt, "MemoryError", "user name allocation failed");
          return CA_UNBOUND;
        }
        struct passwd *entry = getpwnam(username);
        if (entry)
          home = entry->pw_dir;
        free(username);
      }
      if (!home) {
        struct passwd *entry = !user ? getpwuid(getuid()) : NULL;
        home = entry ? entry->pw_dir : NULL;
      }
      if (!home) {
        ca_error(rt, "RuntimeError", "could not determine home directory");
        return CA_UNBOUND;
      }
      char *expanded = join(rt, home, slash ? slash + 1 : "");
      if (!expanded)
        return CA_UNBOUND;
      CaValue result = path(rt, expanded);
      free(expanded);
      return result;
    }
    if (kind == N_PATH_MKDIR) {
      if (out[1].tag != CA_INT_T) {
        ca_error(rt, "TypeError", "mode must be an integer");
        return CA_UNBOUND;
      }
      int error = mkdir_path(name, (mode_t)out[1].as.i, ca_truth(rt, out[2]),
                             ca_truth(rt, out[3]));
      return error ? ca_native_oserror(rt, error, name) : CA_NONE;
    }
    if (kind == N_PATH_READBYTES)
      return read_file(rt, name);
    if (kind == N_PATH_READTEXT) {
      CaValue bytes = read_file(rt, name);
      if (rt->error)
        return CA_UNBOUND;
      CaValue values[] = {bytes, out[1], out[2]};
      CaValue text = ca_text_call(rt, N_TEXT_DECODE, 3, 0, values, NULL);
      if (rt->error)
        return CA_UNBOUND;
      if (out[3].tag == CA_NONE_T) {
        CaValue replace[] = {text, ca_string(rt, "\r\n", 2),
                             ca_string(rt, "\n", 1)};
        text = ca_text_call(rt, N_TEXT_REPLACE, 3, 0, replace, NULL);
        replace[0] = text;
        replace[1] = ca_string(rt, "\r", 1);
        text = ca_text_call(rt, N_TEXT_REPLACE, 3, 0, replace, NULL);
      }
      return text;
    }
    if (kind == N_PATH_WRITEBYTES || kind == N_PATH_WRITETEXT) {
      CaValue data = out[1];
      size_t characters = 0;
      if (kind == N_PATH_WRITETEXT) {
        if (data.tag != CA_STR_T) {
          ca_error(rt, "TypeError", "write_text expects str");
          return CA_UNBOUND;
        }
        CaString *s = (CaString *)data.as.obj;
        for (size_t i = 0; i < s->length; i++)
          if (((unsigned char)s->data[i] & 0xc0) != 0x80)
            characters++;
        CaValue values[] = {data, out[2], out[3]};
        data = ca_text_call(rt, N_TEXT_ENCODE, 3, 0, values, NULL);
        if (rt->error)
          return CA_UNBOUND;
        if (out[4].tag != CA_NONE_T &&
            (out[4].tag != CA_STR_T || ((CaString *)out[4].as.obj)->length)) {
          ca_error(rt, "NotImplementedError",
                   "custom write_text newline translation is not implemented");
          return CA_UNBOUND;
        }
      }
      if (data.tag != CA_BYTES_T) {
        ca_error(rt, "TypeError", "write_bytes expects bytes");
        return CA_UNBOUND;
      }
      CaString *s = (CaString *)data.as.obj;
      FILE *stream = fopen(name, "wb");
      if (!stream)
        return ca_native_oserror(rt, errno, name);
      int error = fwrite(s->data, 1, s->length, stream) != s->length
                      ? (errno ? errno : EIO)
                      : 0;
      if (fclose(stream) && !error)
        error = errno;
      return error ? ca_native_oserror(rt, error, name)
                   : ca_int((int64_t)(kind == N_PATH_WRITETEXT ? characters
                                                               : s->length));
    }
    if (kind == N_PATH_IS_FILE || kind == N_PATH_IS_DIR ||
        kind == N_PATH_EXISTS_METHOD) {
      struct stat st;
      if (stat(name, &st)) {
        if (errno == ENOENT || errno == ENOTDIR)
          return ca_bool(0);
        return ca_native_oserror(rt, errno, name);
      }
      return ca_bool(
          kind == N_PATH_EXISTS_METHOD ||
          (kind == N_PATH_IS_FILE ? S_ISREG(st.st_mode) : S_ISDIR(st.st_mode)));
    }
    if (kind == N_PATH_STAT) {
      struct stat st;
      if (stat(name, &st))
        return ca_native_oserror(rt, errno, name);
      CaValue dict = ca_container(rt, CA_DICT_T, 0, NULL);
      const char *fields[] = {"st_size",  "st_mode", "st_mtime", "st_atime",
                              "st_ctime", "st_ino",  "st_dev",   "st_nlink",
                              "st_uid",   "st_gid"};
      CaValue values[] = {
          ca_int(st.st_size),
          ca_int(st.st_mode),
          ca_float(st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9),
          ca_float(st.st_atim.tv_sec + st.st_atim.tv_nsec / 1e9),
          ca_float(st.st_ctim.tv_sec + st.st_ctim.tv_nsec / 1e9),
          ca_int(st.st_ino),
          ca_int(st.st_dev),
          ca_int(st.st_nlink),
          ca_int(st.st_uid),
          ca_int(st.st_gid)};
      for (size_t i = 0; i < 10 && !rt->error; i++) {
        CaValue key = ca_string(rt, fields[i], strlen(fields[i]));
        ca_setitem(rt, dict, key, values[i]);
      }
      return rt->error ? CA_UNBOUND
                       : ca_native_object(rt, CA_F_STAT, dict, NULL, 0);
    }
    if (kind == N_PATH_RELATIVE) {
      const char *base = ca_native_text(rt, out[1], 1);
      if (!base)
        return CA_UNBOUND;
      size_t count = strlen(base);
      if (!strcmp(base, ".") && name[0] != '/')
        return out[0];
      if (strncmp(name, base, count) ||
          (name[count] && name[count] != '/' && base[count - 1] != '/')) {
        ca_error(rt, "ValueError", "path is not relative to base");
        return CA_UNBOUND;
      }
      const char *relative = name + count;
      while (*relative == '/')
        relative++;
      return path(rt, relative);
    }
    if (kind == N_PATH_RGLOB || kind == N_PATH_ITERDIR) {
      const char *pattern =
          kind == N_PATH_ITERDIR ? "*" : ca_native_text(rt, out[1], 0);
      if (!pattern)
        return CA_UNBOUND;
      if (strchr(pattern, '/')) {
        ca_error(rt, "NotImplementedError",
                 "multi-component native glob patterns are not implemented");
        return CA_UNBOUND;
      }
      Walk *walk = calloc(1, sizeof *walk);
      if (!walk) {
        ca_error(rt, "MemoryError", "directory iterator allocation failed");
        return CA_UNBOUND;
      }
      walk->pattern = strdup(pattern);
      walk->recursive = kind == N_PATH_RGLOB && !glob_only;
      int error = walk->pattern ? walk_push(walk, name) : ENOMEM;
      if (error) {
        free(walk->pattern);
        free(walk);
        return ca_native_oserror(rt, error, name);
      }
      CaValue result =
          ca_native_object(rt, CA_F_DIRECTORY_ITER, out[0], walk, 0);
      if (rt->error) {
        while (walk->top)
          walk_pop(walk);
        free(walk->pattern);
        free(walk);
      }
      return result;
    }
  }
  ca_error(rt, "NotImplementedError",
           "native filesystem API overload is not implemented");
  return CA_UNBOUND;
}
