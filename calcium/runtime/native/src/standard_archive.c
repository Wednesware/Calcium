#define _GNU_SOURCE
#include "dependencies.h"
#ifdef CA_WITH_ARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <sys/stat.h>
#include <unistd.h>
typedef struct {
  struct archive *writer;
  int closed, compression;
} Zip;
static CaValue archive_error(CaRuntime *rt, struct archive *archive) {
  return ca_dep_error(rt, "zipfile.BadZipFile",
                      archive_error_string(archive)
                          ? archive_error_string(archive)
                          : "invalid ZIP archive");
}
static struct archive *reader(CaRuntime *rt, const char *name) {
  struct archive *r = archive_read_new();
  if (!r) {
    ca_error(rt, "MemoryError", "ZIP reader allocation failed");
    return NULL;
  }
  archive_read_support_format_zip(r);
  archive_read_support_filter_all(r);
  if (archive_read_open_filename(r, name, 65536) != ARCHIVE_OK) {
    archive_error(rt, r);
    archive_read_free(r);
    return NULL;
  }
  return r;
}
static CaValue member_info(CaRuntime *rt, struct archive_entry *entry) {
  CaValue v = ca_dep_record(rt, "ZipInfo");
  const char *name = archive_entry_pathname_utf8(entry);
  if (!name)
    name = archive_entry_pathname(entry);
  ca_dep_put(rt, v, "filename", ca_string(rt, name, strlen(name)));
  ca_dep_put(rt, v, "file_size", ca_int(archive_entry_size(entry)));
  return v;
}
static const char *member_name(CaRuntime *rt, CaValue value) {
  if (value.tag == CA_INSTANCE_T)
    value = ca_dep_get(value, "filename");
  return ca_native_text(rt, value, 0);
}
/* Open every directory relative to a trusted descriptor. Existing symlinks are
   never followed, including a malicious archive entry's intermediate parents.
 */
static int extraction_file(CaRuntime *rt, const char *root, const char *member,
                           int isdir, char **relative) {
  char *copy = strdup(member), *clean = calloc(strlen(member) + 1, 1);
  if (!copy || !clean) {
    free(copy);
    free(clean);
    ca_error(rt, "MemoryError", "ZIP path allocation failed");
    return -1;
  }
  size_t at = 0;
  char *save = NULL;
  for (char *part = strtok_r(copy, "/", &save); part;
       part = strtok_r(NULL, "/", &save)) {
    if (!strcmp(part, ".") || !strcmp(part, "..") || !*part)
      continue;
    if (at)
      clean[at++] = '/';
    size_t n = strlen(part);
    memcpy(clean + at, part, n);
    at += n;
  }
  free(copy);
  if (!at) {
    free(clean);
    ca_error(rt, "ValueError", "empty ZIP member path");
    return -1;
  }
  /* The destination is caller-selected. Its final component must be a
     directory, and member traversal below it uses O_NOFOLLOW throughout. */
  CaValue path = ca_path_new(rt, root),
          args[] = {path, ca_int(0777), ca_bool(1), ca_bool(1)};
  ca_io_call(rt, N_PATH_MKDIR, 4, 0, args, NULL);
  if (rt->error) {
    free(clean);
    return -1;
  }
  int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    ca_native_oserror(rt, errno, root);
    free(clean);
    return -1;
  }
  copy = strdup(clean);
  if (!copy) {
    close(fd);
    free(clean);
    ca_error(rt, "MemoryError", "ZIP path allocation failed");
    return -1;
  }
  save = NULL;
  char *part = strtok_r(copy, "/", &save);
  int out = -1;
  while (part) {
    char *next = strtok_r(NULL, "/", &save);
    if (next || isdir) {
      if (mkdirat(fd, part, 0777) < 0 && errno != EEXIST) {
        ca_native_oserror(rt, errno, part);
        break;
      }
      int child =
          openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (child < 0) {
        ca_native_oserror(rt, errno, part);
        break;
      }
      close(fd);
      fd = child;
      if (!next) {
        out = dup(fd);
        break;
      }
    } else {
      out = openat(fd, part,
                   O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0666);
      if (out < 0)
        ca_native_oserror(rt, errno, part);
      break;
    }
    part = next;
  }
  close(fd);
  free(copy);
  if (rt->error) {
    if (out >= 0)
      close(out);
    free(clean);
    return -1;
  }
  *relative = clean;
  return out;
}
static CaValue scan(CaRuntime *rt, CaValue v, int id, CaValue requested,
                    CaValue path, CaValue members) {
  Zip *z = ((CaForeign *)v.as.obj)->handle;
  if (z->closed)
    return ca_dep_error(rt, "ValueError", "ZIP archive is closed");
  if (z->writer)
    return ca_dep_error(rt, "ValueError", "cannot read ZIP while writing");
  const char *filename = ca_native_text(rt, ca_dep_get(v, "filename"), 1);
  if (!filename)
    return CA_UNBOUND;
  struct archive *r = reader(rt, filename);
  if (!r)
    return CA_UNBOUND;
  const char *wanted =
                 requested.tag == CA_NONE_T ? NULL : member_name(rt, requested),
             *destination = NULL;
  if (rt->error) {
    archive_read_free(r);
    return CA_UNBOUND;
  }
  if (id == D_ARCHIVE_EXTRACT || id == D_ARCHIVE_EXTRACTALL) {
    destination = path.tag == CA_NONE_T ? "." : ca_native_text(rt, path, 1);
    if (!destination) {
      archive_read_free(r);
      return CA_UNBOUND;
    }
  }
  CaValue roots[] = {
      v,      requested, path, members, ca_container(rt, CA_LIST_T, 0, NULL),
      CA_NONE};
  CaFrame f;
  ca_push(rt, &f, roots, 6);
  int found = 0, status;
  struct archive_entry *entry;
  while ((status = archive_read_next_header(r, &entry)) == ARCHIVE_OK &&
         !rt->error) {
    const char *name = archive_entry_pathname_utf8(entry);
    if (!name)
      name = archive_entry_pathname(entry);
    if (!name) {
      ca_error(rt, "zipfile.BadZipFile", "ZIP entry has no filename");
      break;
    }
    if (wanted && strcmp(wanted, name)) {
      archive_read_data_skip(r);
      continue;
    }
    if (members.tag != CA_NONE_T) {
      CaValue key = ca_string(rt, name, strlen(name));
      if (!ca_contains(rt, members, key)) {
        archive_read_data_skip(r);
        continue;
      }
    }
    found = 1;
    if (id == D_ARCHIVE_NAMELIST)
      ca_append(rt, roots[4], ca_string(rt, name, strlen(name)));
    else if (id == D_ARCHIVE_INFOLIST)
      ca_append(rt, roots[4], member_info(rt, entry));
    else if (id == D_ARCHIVE_GETINFO) {
      roots[5] = member_info(rt, entry);
      break;
    } else if (id == D_ARCHIVE_READ || id == D_ARCHIVE_TESTZIP) {
      char *buffer = NULL;
      size_t length = 0, capacity = 0;
      char block[65536];
      la_ssize_t got;
      while ((got = archive_read_data(r, block, sizeof block)) > 0) {
        if ((size_t)got > SIZE_MAX - length) {
          ca_error(rt, "MemoryError", "ZIP member too large");
          break;
        }
        if (length + (size_t)got > capacity) {
          capacity = (length + (size_t)got) * 2;
          char *next = realloc(buffer, capacity);
          if (!next) {
            ca_error(rt, "MemoryError", "ZIP member allocation failed");
            break;
          }
          buffer = next;
        }
        memcpy(buffer + length, block, (size_t)got);
        length += (size_t)got;
      }
      if (got < 0 && !rt->error) {
        if (id == D_ARCHIVE_TESTZIP)
          roots[5] = ca_string(rt, name, strlen(name));
        else
          archive_error(rt, r);
      } else if (!rt->error && id == D_ARCHIVE_READ)
        roots[5] = ca_bytes(rt, buffer, length);
      free(buffer);
      if (id == D_ARCHIVE_READ || roots[5].tag != CA_NONE_T)
        break;
    } else if (destination) {
      if (archive_entry_filetype(entry) != AE_IFREG &&
          archive_entry_filetype(entry) != AE_IFDIR) {
        ca_error(rt, "NotImplementedError",
                 "ZIP extraction does not create symlinks or special files");
        break;
      }
      char *relative = NULL;
      int fd =
          extraction_file(rt, destination, name,
                          archive_entry_filetype(entry) == AE_IFDIR, &relative);
      if (fd < 0)
        break;
      char block[65536];
      la_ssize_t got = 0;
      if (archive_entry_filetype(entry) == AE_IFREG)
        while ((got = archive_read_data(r, block, sizeof block)) > 0 &&
               !rt->error) {
          size_t at = 0;
          while (at < (size_t)got) {
            ssize_t wrote = write(fd, block + at, (size_t)got - at);
            if (wrote < 0 && errno == EINTR)
              continue;
            if (wrote <= 0) {
              ca_native_oserror(rt, errno, NULL);
              break;
            }
            at += (size_t)wrote;
          }
        }
      close(fd);
      if (got < 0 && !rt->error)
        archive_error(rt, r);
      if (id == D_ARCHIVE_EXTRACT && !rt->error) {
        char *full = NULL;
        if (asprintf(&full, "%s/%s", destination, relative) < 0)
          ca_error(rt, "MemoryError", "ZIP path allocation failed");
        else {
          roots[5] = ca_string(rt, full, strlen(full));
          free(full);
        }
      }
      free(relative);
      if (id == D_ARCHIVE_EXTRACT)
        break;
    }
    archive_read_data_skip(r);
    ca_safepoint(rt);
  }
  if (!rt->error && status != ARCHIVE_OK && status != ARCHIVE_EOF)
    archive_error(rt, r);
  if (!rt->error && wanted && !found)
    ca_error(rt, "KeyError", wanted);
  if (id == D_ARCHIVE_NAMELIST || id == D_ARCHIVE_INFOLIST)
    roots[5] = roots[4];
  CaValue result = roots[5];
  archive_read_free(r);
  ca_pop(rt, &f);
  return rt->error ? CA_UNBOUND : result;
}
static CaValue archive_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                            const char **k) {
  if (id == D_ARCHIVE_IS_ZIPFILE) {
    if (n != 1 || nkw)
      goto bad;
    const char *filename = ca_native_text(rt, a[0], 1);
    if (!filename)
      return CA_UNBOUND;
    struct archive *r = archive_read_new();
    if (!r)
      return ca_dep_error(rt, "MemoryError", "ZIP reader allocation failed");
    archive_read_support_format_zip(r);
    int status = archive_read_open_filename(r, filename, 65536);
    if (status == ARCHIVE_OK) {
      struct archive_entry *entry;
      status = archive_read_next_header(r, &entry);
    }
    int valid =
        (status == ARCHIVE_OK || status == ARCHIVE_EOF) &&
        ((archive_format(r) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_ZIP);
    archive_read_free(r);
    return ca_bool(valid);
  }

  if (id == D_ARCHIVE_ZIPFILE) {
    CaValue out[4],
        d[] = {CA_NONE, ca_string(rt, "r", 1), ca_int(0), ca_bool(1)};
    const char *names[] = {"file", "mode", "compression", "allowZip64"};
    if (!ca_native_arguments(rt, n, nkw, a, k, 4, 1, names, d, out))
      return CA_UNBOUND;
    const char *filename = ca_native_text(rt, out[0], 1),
               *mode = ca_native_text(rt, out[1], 0);
    if (!filename || !mode)
      return CA_UNBOUND;
    if (strcmp(mode, "r") && strcmp(mode, "w") && strcmp(mode, "x"))
      return ca_dep_error(rt, "NotImplementedError",
                          "native ZIP supports r/w/x modes");
    if (out[2].tag != CA_INT_T || (out[2].as.i != 0 && out[2].as.i != 8))
      return ca_dep_error(rt, "NotImplementedError",
                          "native ZIP supports stored and deflated members");
    Zip *z = calloc(1, sizeof *z);
    if (!z)
      return ca_dep_error(rt, "MemoryError", "ZIP state allocation failed");
    z->compression = (int)out[2].as.i;
    if (*mode == 'r') {
      struct archive *r = reader(rt, filename);
      if (!r) {
        free(z);
        return CA_UNBOUND;
      }
      struct archive_entry *e;
      int status = archive_read_next_header(r, &e);
      if (status != ARCHIVE_OK && status != ARCHIVE_EOF)
        archive_error(rt, r);
      archive_read_free(r);
      if (rt->error) {
        free(z);
        return CA_UNBOUND;
      }
    } else {
      int fd = open(filename,
                    O_WRONLY | O_CREAT | O_CLOEXEC |
                        (*mode == 'x' ? O_EXCL : O_TRUNC),
                    0666);
      if (fd < 0) {
        free(z);
        return ca_native_oserror(rt, errno, filename);
      }
      z->writer = archive_write_new();
      archive_write_set_format_zip(z->writer);
      archive_write_set_format_option(z->writer, "zip", "compression",
                                      z->compression == 8 ? "deflate"
                                                          : "store");
      if (archive_write_open_fd(z->writer, fd) != ARCHIVE_OK) {
        archive_error(rt, z->writer);
        archive_write_free(z->writer);
        close(fd);
        free(z);
        return CA_UNBOUND;
      } /* libarchive does not own the fd; retain it in auxiliary. */
      CaValue state = ca_container(rt, CA_DICT_T, 0, NULL),
              v = ca_native_object(rt, CA_F_ZIPFILE, state, z, fd);
      if (rt->error) {
        archive_write_free(z->writer);
        close(fd);
        free(z);
        return CA_UNBOUND;
      }
      ca_dep_put(rt, v, "filename", out[0]);
      return v;
    }
    CaValue state = ca_container(rt, CA_DICT_T, 0, NULL),
            v = ca_native_object(rt, CA_F_ZIPFILE, state, z, -1);
    if (rt->error) {
      free(z);
      return CA_UNBOUND;
    }
    ca_dep_put(rt, v, "filename", out[0]);
    return v;
  }
  if (n < 1 || a[0].tag != CA_FOREIGN_T ||
      ((CaForeign *)a[0].as.obj)->kind != CA_F_ZIPFILE)
    goto bad;
  CaForeign *f = (CaForeign *)a[0].as.obj;
  Zip *z = f->handle;
  if (id == D_ARCHIVE_CLOSE || id == D_ARCHIVE_EXIT) {
    if (nkw || (id == D_ARCHIVE_CLOSE ? n != 1 : n != 4))
      goto bad;
    if (!z->closed) {
      if (z->writer) {
        if (archive_write_close(z->writer) != ARCHIVE_OK)
          archive_error(rt, z->writer);
        archive_write_free(z->writer);
        z->writer = NULL;
        if (f->auxiliary >= 0) {
          close((int)f->auxiliary);
          f->auxiliary = -1;
        }
      }
      z->closed = 1;
    }
    return rt->error ? CA_UNBOUND : CA_NONE;
  }
  if (z->closed)
    return ca_dep_error(rt, "ValueError", "ZIP archive is closed");
  if (id == D_ARCHIVE_ENTER) {
    if (n != 1 || nkw)
      goto bad;
    return a[0];
  }
  if (id == D_ARCHIVE_WRITESTR || id == D_ARCHIVE_WRITE) {
    CaValue out[4], d[] = {CA_NONE, CA_NONE, CA_NONE, CA_NONE};
    const char *names[] = {
        "self", id == D_ARCHIVE_WRITE ? "filename" : "zinfo_or_arcname",
        id == D_ARCHIVE_WRITE ? "arcname" : "data", "compress_type"};
    if (!ca_native_arguments(rt, n, nkw, a, k, 4, id == D_ARCHIVE_WRITE ? 2 : 3,
                             names, d, out))
      return CA_UNBOUND;
    if (!z->writer)
      return ca_dep_error(rt, "ValueError", "ZIP is not open for writing");
    const char *name = member_name(
        rt, id == D_ARCHIVE_WRITE ? (out[2].tag == CA_NONE_T ? out[1] : out[2])
                                  : out[1]);
    if (!name)
      return CA_UNBOUND;
    CaValue data = out[2];
    if (id == D_ARCHIVE_WRITE) {
      CaValue path = ca_path_new(rt, ca_native_text(rt, out[1], 1));
      data = ca_io_call(rt, N_PATH_READBYTES, 1, 0, &path, NULL);
    }
    if (rt->error)
      return CA_UNBOUND;
    if (data.tag != CA_STR_T && data.tag != CA_BYTES_T)
      return ca_dep_error(rt, "TypeError", "ZIP data must be bytes or str");
    if (out[3].tag != CA_NONE_T && out[3].as.i != z->compression)
      return ca_dep_error(
          rt, "NotImplementedError",
          "per-member ZIP compression override is not implemented");
    CaString *s = (CaString *)data.as.obj;
    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname_utf8(entry, name);
    archive_entry_set_size(entry, (la_int64_t)s->length);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_mtime(entry, 315532800, 0);
    if (archive_write_header(z->writer, entry) != ARCHIVE_OK)
      archive_error(rt, z->writer);
    size_t at = 0;
    while (!rt->error && at < s->length) {
      la_ssize_t wrote =
          archive_write_data(z->writer, s->data + at, s->length - at);
      if (wrote <= 0) {
        archive_error(rt, z->writer);
        break;
      }
      at += (size_t)wrote;
    }
    archive_entry_free(entry);
    return rt->error ? CA_UNBOUND : CA_NONE;
  }
  if (id == D_ARCHIVE_NAMELIST || id == D_ARCHIVE_INFOLIST ||
      id == D_ARCHIVE_TESTZIP) {
    if (n != 1 || nkw)
      goto bad;
    return scan(rt, a[0], id, CA_NONE, CA_NONE, CA_NONE);
  }
  if (id == D_ARCHIVE_READ || id == D_ARCHIVE_GETINFO) {
    if (n != 2 || nkw)
      goto bad;
    return scan(rt, a[0], id, a[1], CA_NONE, CA_NONE);
  }
  if (id == D_ARCHIVE_EXTRACT || id == D_ARCHIVE_EXTRACTALL) {
    CaValue out[4], d[] = {CA_NONE, CA_NONE, CA_NONE, CA_NONE};
    const char *names[] = {"self", id == D_ARCHIVE_EXTRACT ? "member" : "path",
                           id == D_ARCHIVE_EXTRACT ? "path" : "members", "pwd"};
    if (!ca_native_arguments(rt, n, nkw, a, k, 4,
                             id == D_ARCHIVE_EXTRACT ? 2 : 1, names, d, out))
      return CA_UNBOUND;
    if (out[3].tag != CA_NONE_T)
      return ca_dep_error(rt, "NotImplementedError",
                          "encrypted ZIP extraction is not implemented");
    return scan(rt, a[0], id, id == D_ARCHIVE_EXTRACT ? out[1] : CA_NONE,
                id == D_ARCHIVE_EXTRACT ? out[2] : out[1],
                id == D_ARCHIVE_EXTRACT ? CA_NONE : out[2]);
  }
bad:
  return ca_dep_error(rt, "TypeError", "invalid native ZIP arguments");
}
CaValue ca_archive_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                        const char **k) {
  locale_t locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
  if (!locale)
    return ca_native_oserror(rt, errno, NULL);
  locale_t previous = uselocale(locale);
  CaValue result = archive_call(rt, id, n, nkw, a, k);
  uselocale(previous);
  freelocale(locale);
  return result;
}
CaValue ca_archive_attribute(CaRuntime *rt, CaValue v, const char *name) {
  CaValue own = ca_dep_get(v, name);
  if (own.tag != CA_UNBOUND_T)
    return own;
  const char *names[] = {"read",    "namelist",   "infolist", "getinfo",
                         "extract", "extractall", "write",    "writestr",
                         "close",   "__enter__",  "__exit__", "testzip"};
  int ids[] = {D_ARCHIVE_READ,    D_ARCHIVE_NAMELIST, D_ARCHIVE_INFOLIST,
               D_ARCHIVE_GETINFO, D_ARCHIVE_EXTRACT,  D_ARCHIVE_EXTRACTALL,
               D_ARCHIVE_WRITE,   D_ARCHIVE_WRITESTR, D_ARCHIVE_CLOSE,
               D_ARCHIVE_ENTER,   D_ARCHIVE_EXIT,     D_ARCHIVE_TESTZIP};
  for (int i = 0; i < 12; i++)
    if (!strcmp(name, names[i]))
      return ca_native_method(rt, v, ids[i]);
  return ca_dep_error(rt, "AttributeError", name);
}
void ca_archive_finalize(CaForeign *f) {
  if (f->kind != CA_F_ZIPFILE || !f->handle)
    return;
  Zip *z = f->handle;
  if (z->writer) {
    archive_write_free(z->writer);
    if (f->auxiliary >= 0)
      close((int)f->auxiliary);
  }
  free(z);
  f->handle = NULL;
}
#else
CaValue ca_archive_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                        const char **k) {
  (void)id;
  (void)n;
  (void)nkw;
  (void)a;
  (void)k;
  return ca_dep_error(rt, "ImportError", "native zipfile requires libarchive");
}
CaValue ca_archive_attribute(CaRuntime *rt, CaValue v, const char *name) {
  (void)v;
  return ca_dep_error(rt, "AttributeError", name);
}
void ca_archive_finalize(CaForeign *f) { (void)f; }
#endif
