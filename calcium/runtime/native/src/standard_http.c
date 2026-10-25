#define _GNU_SOURCE
#include "standard.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
typedef struct {
  int fd, threaded, stop;
  double timeout;
  unsigned active;
  char *directory;
  pthread_mutex_t mutex;
  pthread_cond_t idle;
} Server;
typedef struct {
  Server *server;
  int fd;
} Request;
static volatile sig_atomic_t interrupted = 0;
static void interrupt(int signal_number) {
  (void)signal_number;
  interrupted = 1;
}
static int send_all(int fd, const void *buffer, size_t length) {
  const char *p = buffer;
  while (length) {
    ssize_t n = send(fd, p, length, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return 0;
    }
    if (!n)
      return 0;
    p += n;
    length -= (size_t)n;
  }
  return 1;
}
static void response(int fd, int status, const char *reason, const char *extra,
                     const char *body, int head) {
  char header[1024];
  size_t length = body ? strlen(body) : 0;
  int n = snprintf(
      header, sizeof header,
      "HTTP/1.0 %d %s\r\nServer: Calcium/26.9\r\nContent-Type: text/html; "
      "charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n",
      status, reason, length, extra ? extra : "");
  if (n > 0 && (size_t)n < sizeof header && send_all(fd, header, (size_t)n) &&
      !head && body)
    send_all(fd, body, length);
}
static int hex(unsigned char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}
static char *translate(const char *directory, const char *url) {
  size_t n = strcspn(url, "?#");
  char *decoded = malloc(n + 1), *path = malloc(strlen(directory) + n + 3);
  if (!decoded || !path) {
    free(decoded);
    free(path);
    return NULL;
  }
  size_t length = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)url[i];
    if (c == '%' && i + 2 < n && hex((unsigned char)url[i + 1]) >= 0 &&
        hex((unsigned char)url[i + 2]) >= 0) {
      c = (unsigned char)(hex((unsigned char)url[i + 1]) * 16 +
                          hex((unsigned char)url[i + 2]));
      i += 2;
    }
    if (!c) {
      free(decoded);
      free(path);
      errno = EINVAL;
      return NULL;
    }
    decoded[length++] = (char)c;
  }
  decoded[length] = 0;
  strcpy(path, directory);
  size_t root = strlen(path), at = root;
  for (size_t i = 0; i < length;) {
    while (decoded[i] == '/')
      i++;
    size_t start = i;
    while (decoded[i] && decoded[i] != '/')
      i++;
    size_t count = i - start;
    if (!count || (count == 1 && decoded[start] == '.'))
      continue;
    if (count == 2 && decoded[start] == '.' && decoded[start + 1] == '.') {
      while (at > root && path[at - 1] != '/')
        at--;
      if (at > root)
        at--;
      continue;
    }
    path[at++] = '/';
    memcpy(path + at, decoded + start, count);
    at += count;
  }
  path[at] = 0;
  free(decoded);
  return path;
}
static const char *mime(const char *path) {
  const char *s = strrchr(path, '.');
  if (!s)
    return "application/octet-stream";
  return !strcasecmp(s, ".html")   ? "text/html"
         : !strcasecmp(s, ".css")  ? "text/css"
         : !strcasecmp(s, ".js")   ? "text/javascript"
         : !strcasecmp(s, ".json") ? "application/json"
         : !strcasecmp(s, ".svg")  ? "image/svg+xml"
         : !strcasecmp(s, ".png")  ? "image/png"
         : !strcasecmp(s, ".jpg") || !strcasecmp(s, ".jpeg") ? "image/jpeg"
         : !strcasecmp(s, ".wasm") ? "application/wasm"
         : !strcasecmp(s, ".txt")  ? "text/plain"
                                   : "application/octet-stream";
}
static void serve_file(Server *server, int fd) {
  struct timeval timeout = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
  char request[65537];
  size_t used = 0;
  int complete = 0;
  while (used < sizeof request - 1) {
    ssize_t n = recv(fd, request + used, sizeof request - 1 - used, 0);
    if (n <= 0)
      return;
    used += (size_t)n;
    request[used] = 0;
    if (strstr(request, "\r\n\r\n") || strstr(request, "\n\n")) {
      complete = 1;
      break;
    }
  }
  if (!complete) {
    response(fd, 431, "Request Header Fields Too Large", NULL,
             "Request too large", 0);
    return;
  }
  char method[16], url[8192], version[32];
  if (sscanf(request, "%15s %8191s %31s", method, url, version) != 3 ||
      strncmp(version, "HTTP/1.", 7)) {
    response(fd, 400, "Bad Request", NULL, "Bad request", 0);
    return;
  }
  int head = !strcmp(method, "HEAD");
  if (strcmp(method, "GET") && !head) {
    response(fd, 501, "Unsupported method", NULL, "Unsupported method", 0);
    return;
  }
  char *path = translate(server->directory, url);
  if (!path) {
    response(fd, 400, "Bad Request", NULL, "Invalid URL", head);
    return;
  }
  struct stat st;
  if (stat(path, &st) < 0) {
    free(path);
    response(fd, 404, "File not found", NULL, "File not found", head);
    return;
  }
  if (S_ISDIR(st.st_mode)) {
    size_t end = strcspn(url, "?#");
    if (!end || url[end - 1] != '/') {
      char extra[8300];
      int n =
          snprintf(extra, sizeof extra, "Location: %.*s/\r\n", (int)end, url);
      if (n > 0 && (size_t)n < sizeof extra)
        response(fd, 301, "Moved Permanently", extra, "", head);
      free(path);
      return;
    }
    size_t length = strlen(path);
    char *next = realloc(path, length + 12);
    if (!next) {
      free(path);
      return;
    }
    path = next;
    strcpy(path + length, "/index.html");
    if (stat(path, &st) < 0) {
      free(path);
      response(fd, 403, "Forbidden", NULL,
               "Directory listing is not implemented", head);
      return;
    }
  }
  if (!S_ISREG(st.st_mode)) {
    free(path);
    response(fd, 404, "File not found", NULL, "File not found", head);
    return;
  }
  int file = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (file < 0) {
    free(path);
    response(fd, 404, "File not found", NULL, "File not found", head);
    return;
  }
  if (fstat(file, &st) < 0 || !S_ISREG(st.st_mode)) {
    close(file);
    free(path);
    response(fd, 404, "File not found", NULL, "File not found", head);
    return;
  }
  char header[512];
  int n = snprintf(header, sizeof header,
                   "HTTP/1.0 200 OK\r\nServer: Calcium/26.9\r\nContent-Type: "
                   "%s\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n",
                   mime(path), (long long)st.st_size);
  if (n > 0 && (size_t)n < sizeof header && send_all(fd, header, (size_t)n) &&
      !head) {
    char buffer[65536];
    for (;;) {
      ssize_t count = read(file, buffer, sizeof buffer);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0 || !send_all(fd, buffer, (size_t)count))
        break;
    }
  }
  close(file);
  free(path);
}
static void *worker(void *opaque) {
  Request *request = opaque;
  Server *server = request->server;
  serve_file(server, request->fd);
  close(request->fd);
  free(request);
  pthread_mutex_lock(&server->mutex);
  server->active--;
  pthread_cond_broadcast(&server->idle);
  pthread_mutex_unlock(&server->mutex);
  return NULL;
}
static int handle(CaRuntime *rt, Server *server, double timeout) {
  struct pollfd pollfd = {server->fd, POLLIN, 0};
  int milliseconds = timeout<0 ? -1 : timeout>(double) INT_MAX / 1000
                         ? INT_MAX
                         : (int)ceil(timeout * 1000);
  int ready = poll(&pollfd, 1, milliseconds);
  if (ready < 0) {
    if (errno == EINTR)
      return 0;
    ca_native_oserror(rt, errno, NULL);
    return -1;
  }
  if (!ready)
    return 0;
  int fd = accept4(server->fd, NULL, NULL, SOCK_CLOEXEC);
  if (fd < 0) {
    if (errno == EINTR)
      return 0;
    ca_native_oserror(rt, errno, NULL);
    return -1;
  }
  if (!server->threaded) {
    serve_file(server, fd);
    close(fd);
    return 1;
  }
  Request *request = malloc(sizeof *request);
  if (!request) {
    close(fd);
    ca_error(rt, "MemoryError", "HTTP request allocation failed");
    return -1;
  }
  request->server = server;
  request->fd = fd;
  pthread_mutex_lock(&server->mutex);
  server->active++;
  pthread_mutex_unlock(&server->mutex);
  pthread_t thread;
  int error = pthread_create(&thread, NULL, worker, request);
  if (error) {
    pthread_mutex_lock(&server->mutex);
    server->active--;
    pthread_mutex_unlock(&server->mutex);
    free(request);
    close(fd);
    ca_native_oserror(rt, error, NULL);
    return -1;
  }
  pthread_detach(thread);
  return 1;
}
void ca_http_finalize(CaForeign *f) {
  if (f->kind != CA_F_HTTP_SERVER || !f->handle)
    return;
  Server *server = f->handle;
  if (server->fd >= 0)
    close(server->fd);
  pthread_mutex_lock(&server->mutex);
  while (server->active)
    pthread_cond_wait(&server->idle, &server->mutex);
  pthread_mutex_unlock(&server->mutex);
  pthread_mutex_destroy(&server->mutex);
  pthread_cond_destroy(&server->idle);
  free(server->directory);
  free(server);
  f->handle = NULL;
}
CaValue ca_http_call(CaRuntime *rt, int id, int n, int nkw, CaValue *args,
                     const char **keys) {
  if (id == N_HTTP_SERVER || id == N_HTTP_THREAD_SERVER) {
    CaValue out[3], defaults[] = {CA_NONE, CA_NONE, ca_bool(1)};
    const char *names[] = {"server_address", "RequestHandlerClass",
                           "bind_and_activate"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 3, 2, names, defaults,
                             out))
      return CA_UNBOUND;
    if (out[0].tag != CA_TUPLE_T || ((CaContainer *)out[0].as.obj)->length != 2)
      goto bad;
    if (!ca_truth(rt, out[2])) {
      ca_error(rt, "NotImplementedError",
               "deferred HTTP binding is not implemented");
      return CA_UNBOUND;
    }
    CaValue host = ca_capture(out[0], 0), port = ca_capture(out[0], 1),
            handler = out[1], directory = CA_NONE;
    if (host.tag != CA_STR_T || port.tag != CA_INT_T || port.as.i < 0 ||
        port.as.i > 65535)
      goto bad;
    if (handler.tag == CA_FOREIGN_T &&
        ((CaForeign *)handler.as.obj)->kind == CA_F_PARTIAL) {
      CaValue data = ((CaForeign *)handler.as.obj)->state;
      handler = ca_capture(data, 0);
      CaContainer *pos = (CaContainer *)ca_capture(data, 1).as.obj;
      CaValue kw = ca_capture(data, 2);
      CaContainer *kwargs = (CaContainer *)kw.as.obj;
      if (pos->length || kwargs->length > 2) {
        ca_error(rt, "NotImplementedError",
                 "HTTP handler partial only supports directory");
        return CA_UNBOUND;
      }
      directory = ca_namespace_get(kw, "directory");
      if (directory.tag == CA_UNBOUND_T)
        directory = CA_NONE;
    }
    CaModule *m = ca_standard_find(rt, "http.server");
    ca_module_start(rt, m);
    CaValue expected =
        ca_attribute(rt, ca_module_value(m), "SimpleHTTPRequestHandler", 0);
    if (rt->error)
      return CA_UNBOUND;
    if (handler.tag != CA_CLASS_T || handler.as.obj != expected.as.obj) {
      ca_error(
          rt, "NotImplementedError",
          "native HTTP supports SimpleHTTPRequestHandler with optional "
          "partial(directory=...); custom request handlers require --cpython");
      return CA_UNBOUND;
    }
    char *folder = directory.tag == CA_NONE_T ? getcwd(NULL, 0) : NULL;
    if (directory.tag != CA_NONE_T) {
      const char *value = ca_native_text(rt, directory, 1);
      if (!value)
        return CA_UNBOUND;
      folder = strdup(value);
    }
    if (!folder) {
      ca_error(rt, "MemoryError", "HTTP directory allocation failed");
      return CA_UNBOUND;
    }
    struct addrinfo hints = {.ai_family = AF_INET,
                             .ai_socktype = SOCK_STREAM,
                             .ai_flags = AI_PASSIVE},
                    *addresses = NULL;
    char service[16];
    snprintf(service, sizeof service, "%lld", (long long)port.as.i);
    const char *hostname = ca_native_text(rt, host, 0);
    if (!hostname) {
      free(folder);
      return CA_UNBOUND;
    }
    int status =
        getaddrinfo(*hostname ? hostname : NULL, service, &hints, &addresses);
    if (status) {
      free(folder);
      ca_error(rt, "OSError", gai_strerror(status));
      return CA_UNBOUND;
    }
    int fd = -1, last = EINVAL;
    struct addrinfo *selected;
    for (selected = addresses; selected; selected = selected->ai_next) {
      fd = socket(selected->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        last = errno;
        continue;
      }
      int reuse = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
      if (bind(fd, selected->ai_addr, selected->ai_addrlen) == 0 &&
          listen(fd, 128) == 0)
        break;
      last = errno;
      close(fd);
      fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
      free(folder);
      return ca_native_oserror(rt, last, NULL);
    }
    Server *server = calloc(1, sizeof *server);
    if (!server) {
      close(fd);
      free(folder);
      ca_error(rt, "MemoryError", "HTTP server allocation failed");
      return CA_UNBOUND;
    }
    server->fd = fd;
    server->directory = folder;
    server->threaded = id == N_HTTP_THREAD_SERVER;
    server->timeout = -1;
    pthread_mutex_init(&server->mutex, NULL);
    pthread_cond_init(&server->idle, NULL);
    struct sockaddr_in actual;
    socklen_t size = sizeof actual;
    getsockname(fd, (struct sockaddr *)&actual, &size);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &actual.sin_addr, ip, sizeof ip);
    CaValue values[] = {ca_string(rt, ip, strlen(ip)),
                        ca_int(ntohs(actual.sin_port))};
    CaValue address = ca_container(rt, CA_TUPLE_T, 2, values);
    CaValue result =
        rt->error ? CA_UNBOUND
                  : ca_native_object(rt, CA_F_HTTP_SERVER, address, server, 0);
    if (rt->error) {
      CaForeign temporary = {.kind = CA_F_HTTP_SERVER, .handle = server};
      ca_http_finalize(&temporary);
    }
    return result;
  }
  if (n < 1 || args[0].tag != CA_FOREIGN_T ||
      ((CaForeign *)args[0].as.obj)->kind != CA_F_HTTP_SERVER)
    goto bad;
  CaForeign *value = (CaForeign *)args[0].as.obj;
  Server *server = value->handle;
  if (id == N_HTTP_CLOSE) {
    if (n != 1 || nkw)
      goto bad;
    ca_http_finalize(value);
    return CA_NONE;
  }
  if (!server) {
    ca_error(rt, "ValueError", "HTTP server is closed");
    return CA_UNBOUND;
  }
  if (id == N_HTTP_SHUTDOWN) {
    if (n != 1 || nkw)
      goto bad;
    server->stop = 1;
    return CA_NONE;
  }
  if (id == N_HTTP_HANDLE) {
    if (n != 1 || nkw)
      goto bad;
    handle(rt, server, server->timeout);
    return rt->error ? CA_UNBOUND : CA_NONE;
  }
  if (id == N_HTTP_SERVE) {
    CaValue out[2], defaults[] = {CA_NONE, ca_float(0.5)};
    const char *names[] = {"self", "poll_interval"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 2, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    double interval = ca_as_double(rt, out[1]);
    if (rt->error)
      return CA_UNBOUND;
    if (!isfinite(interval) || interval < 0) {
      ca_error(rt, "ValueError", "invalid poll interval");
      return CA_UNBOUND;
    }
    struct sigaction old, action = {0};
    action.sa_handler = interrupt;
    sigemptyset(&action.sa_mask);
    interrupted = 0;
    if (sigaction(SIGINT, &action, &old) < 0)
      return ca_native_oserror(rt, errno, NULL);
    while (!server->stop && !interrupted && !rt->error)
      handle(rt, server, interval);
    sigaction(SIGINT, &old, NULL);
    if (interrupted)
      ca_error(rt, "KeyboardInterrupt", "");
    return rt->error ? CA_UNBOUND : CA_NONE;
  }
bad:
  ca_error(rt, "TypeError", "invalid native HTTP arguments");
  return CA_UNBOUND;
}
CaValue ca_http_attribute(CaRuntime *rt, CaValue value, const char *name) {
  CaForeign *f = (CaForeign *)value.as.obj;
  Server *server = f->handle;
  if (!strcmp(name, "server_address"))
    return f->state;
  if (!strcmp(name, "server_port"))
    return ca_capture(f->state, 1);
  if (!strcmp(name, "server_name"))
    return ca_capture(f->state, 0);
  if (!strcmp(name, "timeout"))
    return server && server->timeout >= 0 ? ca_float(server->timeout) : CA_NONE;
  int id = !strcmp(name, "serve_forever")    ? N_HTTP_SERVE
           : !strcmp(name, "handle_request") ? N_HTTP_HANDLE
           : !strcmp(name, "server_close")   ? N_HTTP_CLOSE
           : !strcmp(name, "shutdown")       ? N_HTTP_SHUTDOWN
                                             : 0;
  if (id)
    return ca_native_method(rt, value, id);
  ca_error(rt, "AttributeError", name);
  return CA_UNBOUND;
}
void ca_http_set(CaRuntime *rt, CaValue value, const char *name, CaValue data) {
  Server *server = ((CaForeign *)value.as.obj)->handle;
  if (strcmp(name, "timeout")) {
    ca_error(rt, "AttributeError", name);
    return;
  }
  if (!server) {
    ca_error(rt, "ValueError", "HTTP server is closed");
    return;
  }
  double timeout = data.tag == CA_NONE_T ? -1 : ca_as_double(rt, data);
  if (!rt->error &&
      (!isfinite(timeout) || (timeout < 0 && data.tag != CA_NONE_T)))
    ca_error(rt, "ValueError", "timeout must be nonnegative");
  if (!rt->error)
    server->timeout = timeout;
}
