#define _GNU_SOURCE
#include "dependencies.h"
#ifdef CA_WITH_CURL
#include <curl/curl.h>
#include <errno.h>
#include <unistd.h>
typedef struct {
  CaRuntime *rt;
  CaValue headers, hook;
  FILE *file;
  char *buffer;
  size_t length, capacity;
  long blocks;
  curl_off_t total;
  int failed, started;
  char reason[128];
} Transfer;
static size_t receive(char *data, size_t size, size_t count, void *context) {
  Transfer *t = context;
  if (size && count > SIZE_MAX / size)
    return 0;
  size_t n = size * count;
  if (t->file) {
    if (fwrite(data, 1, n, t->file) != n) {
      t->failed = errno ? errno : EIO;
      return 0;
    }
  } else {
    if (n > SIZE_MAX - t->length)
      return 0;
    size_t need = t->length + n;
    if (need > t->capacity) {
      size_t cap = need > SIZE_MAX / 2 ? need : need * 2;
      char *next = realloc(t->buffer, cap);
      if (!next) {
        t->failed = ENOMEM;
        return 0;
      }
      t->buffer = next;
      t->capacity = cap;
    }
    memcpy(t->buffer + t->length, data, n);
  }
  t->length += n;
  if (t->hook.tag != CA_NONE_T) {
    if (!t->started) {
      CaValue args[] = {ca_int(0), ca_int(8192), ca_int((int64_t)t->total)};
      t->started = 1;
      ca_call(t->rt, t->hook, 3, 0, args, NULL);
    }
    long blocks = (long)((t->length + 8191) / 8192);
    while (t->blocks < blocks && !t->rt->error) {
      t->blocks++;
      CaValue args[] = {ca_int(t->blocks), ca_int(8192),
                        ca_int((int64_t)t->total)};
      ca_call(t->rt, t->hook, 3, 0, args, NULL);
    }
    if (t->rt->error)
      return 0;
  }
  return n;
}
static size_t header(char *data, size_t size, size_t count, void *context) {
  Transfer *t = context;
  if (size && count > SIZE_MAX / size)
    return 0;
  size_t n = size * count;
  if (n >= 5 &&
      !memcmp(data, "HTTP/",
              5)) { /* Redirect headers replace the preceding response. */
    CaContainer *c = (CaContainer *)t->headers.as.obj;
    c->length = 0;
    t->total = -1;
    const char *space = memchr(data, ' ', n);
    if (space) {
      space = memchr(space + 1, ' ', n - (size_t)(space + 1 - data));
      if (space) {
        size_t len = n - (size_t)(space + 1 - data);
        while (len &&
               (space[1 + len - 1] == '\r' || space[1 + len - 1] == '\n'))
          len--;
        if (len >= sizeof t->reason)
          len = sizeof t->reason - 1;
        memcpy(t->reason, space + 1, len);
        t->reason[len] = 0;
      }
    }
    return n;
  }
  char *colon = memchr(data, ':', n);
  if (colon) {
    size_t keylen = (size_t)(colon - data), start = keylen + 1, end = n;
    while (start < end && (data[start] == ' ' || data[start] == '\t'))
      start++;
    while (end > start && (data[end - 1] == '\r' || data[end - 1] == '\n' ||
                           data[end - 1] == ' '))
      end--;
    char *key = strndup(data, keylen);
    if (!key) {
      t->failed = ENOMEM;
      return 0;
    }
    CaValue value = ca_string(t->rt, data + start, end - start);
    ca_namespace_set(t->rt, t->headers, key, value);
    if (!strcasecmp(key, "Content-Length")) {
      char *number = strndup(data + start, end - start);
      if (number) {
        t->total = (curl_off_t)strtoll(number, NULL, 10);
        free(number);
      }
    }
    free(key);
    if (t->rt->error)
      return 0;
  }
  return n;
}
static CaValue url_error(CaRuntime *rt, long status, const char *url,
                         const char *reason, CaValue headers) {
  CaValue args[] = {ca_string(rt, url, strlen(url)), ca_int(status),
                    ca_string(rt, reason, strlen(reason)), headers, CA_NONE};
  CaValue e = ca_exception_new(
      rt, ca_type(status >= 400 ? CA_T_HTTPERROR : CA_T_URLERROR),
      status >= 400 ? 5 : 1, status >= 400 ? args : args + 2);
  if (!rt->error)
    ca_raise(rt, e, CA_NONE, 0);
  return CA_UNBOUND;
}
CaValue ca_url_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                    const char **k) {
  if (id != D_URL_RETRIEVE && id != D_URL_OPEN) {
    if (n < 1 || a[0].tag != CA_FOREIGN_T ||
        ((CaForeign *)a[0].as.obj)->kind != CA_F_URL)
      goto bad;
    CaForeign *f = (CaForeign *)a[0].as.obj;
    if (id == D_URL_ENTER) {
      if (n != 1 || nkw)
        goto bad;
      if (ca_dep_get(a[0], "closed").as.i)
        return ca_dep_error(rt, "ValueError", "response is closed");
      return a[0];
    }
    if (id == D_URL_CLOSE || id == D_URL_EXIT) {
      if (nkw || (id == D_URL_CLOSE ? n != 1 : n != 4))
        goto bad;
      ca_dep_put(rt, a[0], "closed", ca_bool(1));
      return CA_NONE;
    }
    if (id == D_URL_READ) {
      if (n > 2 || nkw)
        goto bad;
      if (ca_dep_get(a[0], "closed").as.i)
        return ca_dep_error(rt, "ValueError", "response is closed");
      if (n == 2 && a[1].tag != CA_INT_T)
        goto bad;
      CaString *s = (CaString *)ca_dep_get(a[0], "_body").as.obj;
      size_t remaining = s->length - (size_t)f->auxiliary,
             amount = n == 1 || a[1].as.i < 0           ? remaining
                      : (uint64_t)a[1].as.i > remaining ? remaining
                                                        : (size_t)a[1].as.i;
      CaValue value = ca_bytes(rt, s->data + f->auxiliary, amount);
      f->auxiliary += (long)amount;
      return value;
    }
    if (id == D_URL_GETHEADER) {
      if (n < 2 || n > 3 || nkw)
        goto bad;
      const char *name = ca_native_text(rt, a[1], 0);
      if (!name)
        return CA_UNBOUND;
      CaContainer *headers = (CaContainer *)ca_dep_get(a[0], "headers").as.obj;
      for (size_t i = 0; i < headers->length; i += 2)
        if (!strcasecmp(((CaString *)headers->items[i].as.obj)->data, name))
          return headers->items[i + 1];
      return n == 3 ? a[2] : CA_NONE;
    }
    if (n != 1 || nkw)
      goto bad;
    return ca_dep_get(a[0], id == D_URL_GETCODE ? "status" : "url");
  }
  CaValue out[4], d[] = {CA_NONE, CA_NONE, CA_NONE, CA_NONE};
  const char *retrieve[] = {"url", "filename", "reporthook", "data"},
             *open_names[] = {"url", "data", "timeout"};
  if (!ca_native_arguments(rt, n, nkw, a, k, id == D_URL_RETRIEVE ? 4 : 3, 1,
                           id == D_URL_RETRIEVE ? retrieve : open_names, d,
                           out))
    return CA_UNBOUND;
  const char *url = ca_native_text(rt, out[0], 0);
  if (!url)
    return CA_UNBOUND;
  CaValue post = id == D_URL_RETRIEVE ? out[3] : out[1],
          timeout = id == D_URL_RETRIEVE ? CA_NONE : out[2];
  if (post.tag != CA_NONE_T && post.tag != CA_BYTES_T)
    return ca_dep_error(rt, "TypeError", "URL request data must be bytes");
  if (timeout.tag != CA_NONE_T && timeout.tag != CA_INT_T &&
      timeout.tag != CA_FLOAT_T)
    return ca_dep_error(rt, "TypeError", "URL timeout must be numeric");
  CaValue roots[] = {out[0],
                     out[1],
                     post,
                     ca_container(rt, CA_DICT_T, 0, NULL),
                     id == D_URL_RETRIEVE ? out[2] : CA_NONE,
                     CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 6);
  Transfer transfer = {
      .rt = rt, .headers = roots[3], .hook = roots[4], .total = -1};
  char *temporary = NULL;
  const char *filename = NULL;
  int created = 0;
  CURL *curl = NULL;
  CaValue result = CA_UNBOUND;
  if (id == D_URL_RETRIEVE) {
    if (out[1].tag == CA_NONE_T) {
      temporary = strdup("/tmp/calcium-url-XXXXXX");
      int fd = temporary ? mkstemp(temporary) : -1;
      if (fd < 0) {
        ca_native_oserror(rt, temporary ? errno : ENOMEM, NULL);
        goto done;
      }
      transfer.file = fdopen(fd, "wb");
      if (!transfer.file) {
        close(fd);
        ca_native_oserror(rt, errno, NULL);
        goto done;
      }
      filename = temporary;
    } else {
      filename = ca_native_text(rt, out[1], 1);
      if (!filename)
        goto done;
      transfer.file = fopen(filename, "wb");
      if (!transfer.file) {
        ca_native_oserror(rt, errno, filename);
        goto done;
      }
    }
    created = 1;
  }
  curl = curl_easy_init();
  if (!curl) {
    ca_error(rt, "MemoryError", "HTTP transfer allocation failed");
    goto done;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https,file");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Calcium/26.11");
  if (timeout.tag != CA_NONE_T) {
    double seconds =
        timeout.tag == CA_INT_T ? (double)timeout.as.i : timeout.as.f;
    if (!isfinite(seconds) || seconds <= 0) {
      ca_error(rt, "ValueError", "timeout must be positive and finite");
      goto done;
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     (long)fmin((double)LONG_MAX, seconds * 1000));
  }
  if (post.tag == CA_BYTES_T) {
    CaString *data = (CaString *)post.as.obj;
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)data->data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     (curl_off_t)data->length);
  }
  /* Header callbacks create native values, so retain the runtime lock
     throughout this bounded synchronous transfer. Other threads resume at the
     next yield. */
  CURLcode code = curl_easy_perform(curl);
  long status = 0;
  char *effective = NULL;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
  if (rt->error)
    goto done;
  if (transfer.failed) {
    ca_native_oserror(rt, transfer.failed, filename);
    goto done;
  }
  if (code != CURLE_OK || status >= 400) {
    url_error(rt, status, url,
              code != CURLE_OK ? curl_easy_strerror(code)
                               : (transfer.reason[0] ? transfer.reason
                                                     : "HTTP request failed"),
              roots[3]);
    goto done;
  }
  if (transfer.hook.tag != CA_NONE_T && !transfer.started) {
    CaValue args[] = {ca_int(0), ca_int(8192), ca_int((int64_t)transfer.total)};
    ca_call(rt, transfer.hook, 3, 0, args, NULL);
    if (rt->error)
      goto done;
  }
  if (id == D_URL_RETRIEVE) {
    if (fflush(transfer.file) != 0) {
      ca_native_oserror(rt, errno, filename);
      goto done;
    }
    roots[5] = ca_string(rt, filename, strlen(filename));
    result = ca_container(rt, CA_TUPLE_T, 2, (CaValue[]){roots[5], roots[3]});
  } else {
    roots[5] = ca_native_object(rt, CA_F_URL,
                                ca_container(rt, CA_DICT_T, 0, NULL), NULL, 0);
    ca_dep_put(rt, roots[5], "_body",
               ca_bytes(rt, transfer.buffer, transfer.length));
    ca_dep_put(rt, roots[5], "status", status ? ca_int(status) : CA_NONE);
    ca_dep_put(rt, roots[5], "code", status ? ca_int(status) : CA_NONE);
    ca_dep_put(rt, roots[5], "url",
               ca_string(rt, effective ? effective : url,
                         strlen(effective ? effective : url)));
    ca_dep_put(rt, roots[5], "headers", roots[3]);
    ca_dep_put(rt, roots[5], "closed", ca_bool(0));
    result = roots[5];
  }
done:
  if (curl)
    curl_easy_cleanup(curl);
  if (transfer.file && fclose(transfer.file) != 0 && !rt->error)
    ca_native_oserror(rt, errno, filename);
  if (rt->error && created && filename)
    unlink(filename);
  free(temporary);
  free(transfer.buffer);
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : result;
bad:
  return ca_dep_error(rt, "TypeError", "invalid native URL arguments");
}
CaValue ca_url_attribute(CaRuntime *rt, CaValue v, const char *name) {
  CaValue own = ca_dep_get(v, name);
  if (own.tag != CA_UNBOUND_T && name[0] != '_')
    return own;
  const char *names[] = {"read",    "close",  "__enter__", "__exit__",
                         "getcode", "geturl", "getheader"};
  int ids[] = {D_URL_READ,    D_URL_CLOSE,  D_URL_ENTER,    D_URL_EXIT,
               D_URL_GETCODE, D_URL_GETURL, D_URL_GETHEADER};
  for (int i = 0; i < 7; i++)
    if (!strcmp(name, names[i]))
      return ca_native_method(rt, v, ids[i]);
  return ca_dep_error(rt, "AttributeError", name);
}
#else
CaValue ca_url_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                    const char **k) {
  (void)id;
  (void)n;
  (void)nkw;
  (void)a;
  (void)k;
  return ca_dep_error(rt, "ImportError",
                      "native urllib.request requires libcurl");
}
CaValue ca_url_attribute(CaRuntime *rt, CaValue v, const char *name) {
  (void)v;
  return ca_dep_error(rt, "AttributeError", name);
}
#endif
