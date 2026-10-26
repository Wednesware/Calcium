#define _GNU_SOURCE
#include "dependencies.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
typedef struct {
  pid_t pid;
  int fd[3], done, status, text;
  CaForeign *streams[3];
  char *buffer[2];
  size_t used[2], capacity[2];
} Process;
static void close_pipe(Process *p, int i) {
  if (p->streams[i]) {
    if (p->streams[i]->handle)
      ca_file_close(p->streams[i]);
  } else if (p->fd[i] >= 0)
    close(p->fd[i]);
  p->fd[i] = -1;
}
static double clock_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void free_strings(char **v) {
  if (v) {
    for (size_t i = 0; v[i]; i++)
      free(v[i]);
    free(v);
  }
}
static char **argv_values(CaRuntime *rt, CaValue value) {
  if (value.tag == CA_STR_T) {
    const char *s = ca_native_text(rt, value, 0);
    char **v = calloc(2, sizeof(char *));
    if (v && s)
      v[0] = strdup(s);
    if (!v || !v[0]) {
      free_strings(v);
      if (!rt->error)
        ca_error(rt, "MemoryError", "process arguments allocation failed");
      return NULL;
    }
    return v;
  }
  CaValue roots[] = {value, ca_iter(rt, value), CA_UNBOUND};
  CaFrame f;
  ca_push(rt, &f, roots, 3);
  size_t length = 0, capacity = 8;
  char **v = calloc(capacity, sizeof(char *));
  if (!v)
    ca_error(rt, "MemoryError", "process arguments allocation failed");
  while (!rt->error && ca_next(rt, roots[1], &roots[2]) > 0) {
    const char *s = ca_native_text(rt, roots[2], 1);
    if (!s)
      break;
    if (length + 1 >= capacity) {
      capacity *= 2;
      char **next = realloc(v, capacity * sizeof(char *));
      if (!next) {
        ca_error(rt, "MemoryError", "process arguments allocation failed");
        break;
      }
      v = next;
      memset(v + length, 0, (capacity - length) * sizeof(char *));
    }
    v[length] = strdup(s);
    if (!v[length]) {
      ca_error(rt, "MemoryError", "process arguments allocation failed");
      break;
    }
    length++;
  }
  ca_pop(rt, &f);
  if (rt->error || !length) {
    free_strings(v);
    if (!rt->error)
      ca_error(rt, "IndexError", "empty process arguments");
    return NULL;
  }
  return v;
}
static char **env_values(CaRuntime *rt, CaValue value) {
  if (value.tag == CA_NONE_T)
    return NULL;
  if (value.tag == CA_FOREIGN_T &&
      ((CaForeign *)value.as.obj)->kind == CA_F_ENVIRON)
    return NULL;
  if (value.tag != CA_DICT_T) {
    ca_error(rt, "TypeError", "env must be a mapping");
    return NULL;
  }
  CaContainer *c = (CaContainer *)value.as.obj;
  char **v = calloc(c->length / 2 + 1, sizeof(char *));
  if (!v) {
    ca_error(rt, "MemoryError", "process environment allocation failed");
    return NULL;
  }
  for (size_t i = 0; i < c->length && !rt->error; i += 2) {
    const char *key = ca_native_text(rt, c->items[i], 0),
               *val = ca_native_text(rt, c->items[i + 1], 0);
    if (!key || !val)
      break;
    if (strchr(key, '=')) {
      ca_error(rt, "ValueError", "illegal environment variable name");
      break;
    }
    if (asprintf(&v[i / 2], "%s=%s", key, val) < 0)
      ca_error(rt, "MemoryError", "process environment allocation failed");
  }
  if (rt->error) {
    free_strings(v);
    return NULL;
  }
  return v;
}
static char *resolve(const char *name, char **env) {
  if (strchr(name, '/'))
    return strdup(name);
  const char *path = NULL;
  for (size_t i = 0; env && env[i]; i++)
    if (!strncmp(env[i], "PATH=", 5)) {
      path = env[i] + 5;
      break;
    }
  if (!path)
    path = "/bin:/usr/bin";
  const char *at = path;
  char *fallback = NULL;
  for (;;) {
    const char *end = strchr(at, ':');
    size_t len = end ? (size_t)(end - at) : strlen(at);
    char *file = NULL;
    if (asprintf(&file, "%.*s%s%s", (int)len, at, len ? "/" : "", name) < 0) {
      free(fallback);
      return NULL;
    }
    if (access(file, X_OK) == 0) {
      free(fallback);
      return file;
    }
    if (errno == EACCES && !fallback)
      fallback = strdup(file);
    free(file);
    if (!end)
      break;
    at = end + 1;
  }
  return fallback ? fallback : strdup(name);
}
static int target_fd(CaRuntime *rt, CaValue v) {
  if (v.tag == CA_NONE_T)
    return -4;
  if (v.tag == CA_INT_T && v.as.i >= -3 && v.as.i <= INT_MAX)
    return (int)v.as.i;
  CaValue method = ca_attribute(rt, v, "fileno", 0);
  CaValue fd = rt->error ? CA_UNBOUND : ca_call(rt, method, 0, 0, NULL, NULL);
  if (rt->error)
    return -5;
  if (fd.tag != CA_INT_T || fd.as.i < 0 || fd.as.i > INT_MAX) {
    ca_error(rt, "ValueError", "invalid process file descriptor");
    return -5;
  }
  return (int)fd.as.i;
}
static void poll_status(Process *p, int block) {
  if (p->done)
    return;
  int status;
  pid_t r;
  do {
    r = waitpid(p->pid, &status, block ? 0 : WNOHANG);
  } while (r < 0 && errno == EINTR);
  if (r == p->pid) {
    p->done = 1;
    p->status = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
  }
}
static CaValue spawn(CaRuntime *rt, int n, int nkw, CaValue *a,
                     const char **keys, int run, CaValue *check,
                     CaValue *timeout, CaValue *input) {
  const char *names[] = {"args",
                         "stdin",
                         "stdout",
                         "stderr",
                         "shell",
                         "cwd",
                         "env",
                         "text",
                         "encoding",
                         "errors",
                         "universal_newlines",
                         "bufsize",
                         "close_fds",
                         "start_new_session",
                         "pass_fds",
                         "capture_output",
                         "check",
                         "timeout",
                         "input"};
  CaValue d[] = {CA_NONE,    CA_NONE,    CA_NONE,    CA_NONE,    ca_bool(0),
                 CA_NONE,    CA_NONE,    ca_bool(0), CA_NONE,    CA_NONE,
                 CA_NONE,    ca_int(-1), ca_bool(1), ca_bool(0), CA_NONE,
                 ca_bool(0), ca_bool(0), CA_NONE,    CA_NONE},
          out[19];
  if (!ca_native_arguments(rt, n, nkw, a, keys, run ? 19 : 15, 1, names, d,
                           out))
    return CA_UNBOUND;
  if (!ca_truth(rt, out[12]) || ca_truth(rt, out[13]) ||
      out[14].tag != CA_NONE_T)
    return ca_dep_error(rt, "NotImplementedError",
                        "native subprocess requires close_fds=True and no "
                        "pass_fds or new session");
  int text =
      ca_truth(rt, out[7]) || ca_truth(rt, out[10]) || out[8].tag != CA_NONE_T;
  if (out[8].tag != CA_NONE_T) {
    const char *enc = ca_native_text(rt, out[8], 0);
    if (!enc)
      return CA_UNBOUND;
    if (strcmp(enc, "utf-8") && strcmp(enc, "utf8") && strcmp(enc, "UTF-8"))
      return ca_dep_error(rt, "NotImplementedError",
                          "native subprocess supports UTF-8 encoding");
  }
  if (out[9].tag != CA_NONE_T) {
    const char *policy = ca_native_text(rt, out[9], 0);
    if (!policy)
      return CA_UNBOUND;
    if (strcmp(policy, "strict"))
      return ca_dep_error(rt, "NotImplementedError",
                          "native subprocess supports strict decoding");
  }
  if (run) {
    *check = out[16];
    *timeout = out[17];
    *input = out[18];
    if (ca_truth(rt, out[15])) {
      if (out[2].tag != CA_NONE_T || out[3].tag != CA_NONE_T)
        return ca_dep_error(rt, "ValueError",
                            "capture_output conflicts with stdout/stderr");
      out[2] = out[3] = ca_int(-1);
    }
    if (input->tag != CA_NONE_T) {
      if (out[1].tag != CA_NONE_T)
        return ca_dep_error(rt, "ValueError",
                            "stdin and input cannot both be used");
      out[1] = ca_int(-1);
    }
  }
  char **argv = argv_values(rt, out[0]),
       **env = rt->error ? NULL : env_values(rt, out[6]);
  if (rt->error) {
    free_strings(argv);
    free_strings(env);
    return CA_UNBOUND;
  }
  char *shell_argv[4] = {"/bin/sh", "-c", NULL, NULL};
  char *file = NULL;
  if (ca_truth(rt, out[4])) {
    if (out[0].tag != CA_STR_T) {
      free_strings(argv);
      free_strings(env);
      return ca_dep_error(rt, "NotImplementedError",
                          "native shell=True requires a string command");
    }
    shell_argv[2] = argv[0];
    file = strdup("/bin/sh");
  } else
    file = resolve(argv[0], env ? env : environ);
  if (!file) {
    free_strings(argv);
    free_strings(env);
    return ca_dep_error(rt, "MemoryError", "process path allocation failed");
  }
  Process *p = calloc(1, sizeof *p);
  if (!p) {
    free(file);
    free_strings(argv);
    free_strings(env);
    return ca_dep_error(rt, "MemoryError", "process state allocation failed");
  }
  for (int i = 0; i < 3; i++)
    p->fd[i] = -1;
  p->text = text;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  int child[3] = {-1, -1, -1}, err = 0;
  for (int i = 0; i < 3 && !rt->error; i++) {
    int fd = target_fd(rt, out[1 + i]);
    if (fd == -1) {
      int pipefd[2];
      if (pipe2(pipefd, O_CLOEXEC)) {
        ca_native_oserror(rt, errno, NULL);
        break;
      }
      p->fd[i] = pipefd[i ? 0 : 1];
      child[i] = pipefd[i ? 1 : 0];
      posix_spawn_file_actions_adddup2(&actions, child[i], i);
    } else if (fd == -3) {
      child[i] = open("/dev/null", (i ? O_WRONLY : O_RDONLY) | O_CLOEXEC);
      if (child[i] < 0) {
        ca_native_oserror(rt, errno, NULL);
        break;
      }
      posix_spawn_file_actions_adddup2(&actions, child[i], i);
    } else if (fd == -2 && i == 2)
      posix_spawn_file_actions_adddup2(&actions, 1, 2);
    else if (fd >= 0)
      posix_spawn_file_actions_adddup2(&actions, fd, i);
    else if (fd != -4 && !rt->error)
      ca_error(rt, "ValueError", "invalid process redirection");
  }
  if (out[5].tag != CA_NONE_T && !rt->error) {
    const char *cwd = ca_native_text(rt, out[5], 1);
    if (cwd)
      err = posix_spawn_file_actions_addchdir_np(&actions, cwd);
  }
  if (!rt->error && !err)
    err = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  if (!rt->error && !err)
    err = posix_spawn(&p->pid, file, &actions, NULL,
                      ca_truth(rt, out[4]) ? shell_argv : argv,
                      env ? env : environ);
  posix_spawn_file_actions_destroy(&actions);
  for (int i = 0; i < 3; i++)
    if (child[i] >= 0)
      close(child[i]);
  free(file);
  free_strings(argv);
  free_strings(env);
  if (err && !rt->error)
    ca_native_oserror(rt, err, NULL);
  if (rt->error) {
    for (int i = 0; i < 3; i++)
      if (p->fd[i] >= 0)
        close(p->fd[i]);
    free(p);
    return CA_UNBOUND;
  }
  CaValue state = ca_container(rt, CA_DICT_T, 0, NULL),
          v = ca_native_object(rt, CA_F_PROCESS, state, p, 0);
  if (rt->error) {
    kill(p->pid, SIGKILL);
    waitpid(p->pid, NULL, 0);
    for (int i = 0; i < 3; i++)
      if (p->fd[i] >= 0)
        close(p->fd[i]);
    free(p);
    return CA_UNBOUND;
  }
  ca_dep_put(rt, v, "args", out[0]);
  ca_dep_put(rt, v, "pid", ca_int(p->pid));
  return v;
}
static int add_buffer(Process *p, int which, const char *text, size_t n) {
  if (n > SIZE_MAX - p->used[which] - 1)
    return 0;
  size_t need = p->used[which] + n + 1;
  if (need > p->capacity[which]) {
    size_t cap = need > SIZE_MAX / 2 ? need : need * 2;
    char *grown = realloc(p->buffer[which], cap);
    if (!grown)
      return 0;
    p->buffer[which] = grown;
    p->capacity[which] = cap;
  }
  memcpy(p->buffer[which] + p->used[which], text, n);
  p->used[which] += n;
  return 1;
}
static CaValue communicate(CaRuntime *rt, CaValue v, CaValue input,
                           CaValue timeout, int kill_timeout) {
  Process *p = ((CaForeign *)v.as.obj)->handle;
  double duration = -1;
  if (timeout.tag != CA_NONE_T) {
    if (timeout.tag != CA_INT_T && timeout.tag != CA_FLOAT_T)
      return ca_dep_error(rt, "TypeError", "timeout must be numeric");
    duration = timeout.tag == CA_INT_T ? (double)timeout.as.i : timeout.as.f;
  }
  if (input.tag != CA_NONE_T && input.tag != (p->text ? CA_STR_T : CA_BYTES_T))
    return ca_dep_error(rt, "TypeError",
                        "process input type does not match text mode");
  if (input.tag != CA_NONE_T && p->fd[0] < 0)
    return ca_dep_error(rt, "ValueError", "stdin is not a pipe");
  size_t sent = 0;
  CaString *data = input.tag == CA_NONE_T ? NULL : (CaString *)input.as.obj;
  double deadline = clock_now() + duration;
  int timed = 0, failed = 0, code = 0;
  for (int i = 0; i < 3; i++)
    if (p->streams[i])
      p->fd[i] = p->streams[i]->handle ? fileno(p->streams[i]->handle) : -1;
  for (int i = 0; i < 3; i++)
    if (p->fd[i] >= 0)
      fcntl(p->fd[i], F_SETFL, fcntl(p->fd[i], F_GETFL) | O_NONBLOCK);
  /* SIGPIPE is blocked on this thread during writes; consume only signals
   * produced here. */
  sigset_t blocked, old, pending;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGPIPE);
  sigpending(&pending);
  int had_pipe = sigismember(&pending, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &blocked, &old);
  ca_thread_release(rt);
  for (;;) {
    poll_status(p, 0);
    if (p->fd[0] >= 0 && (!data || sent == data->length)) {
      close_pipe(p, 0);
    }
    if (p->done && p->fd[1] < 0 && p->fd[2] < 0)
      break;
    if (duration >= 0 && clock_now() >= deadline) {
      timed = 1;
      if (kill_timeout) {
        kill(p->pid, SIGKILL);
        poll_status(p, 1);
      }
      break;
    }
    struct pollfd fds[3];
    for (int i = 0; i < 3; i++)
      fds[i] = (struct pollfd){.fd = p->fd[i], .events = i ? POLLIN : POLLOUT};
    int wait = duration < 0
                   ? 50
                   : (int)fmax(0, fmin(50, (deadline - clock_now()) * 1000));
    int r = poll(fds, 3, wait);
    if (r < 0 && errno != EINTR) {
      failed = 1;
      code = errno;
      break;
    }
    for (int i = 0; i < 3 && !failed; i++)
      if (p->fd[i] >= 0 && fds[i].revents) {
        if (!i) {
          ssize_t wrote =
              write(p->fd[0], data->data + sent, data->length - sent);
          if (wrote > 0)
            sent += (size_t)wrote;
          else if (wrote < 0 && errno != EINTR && errno != EAGAIN) {
            if (errno != EPIPE) {
              failed = 1;
              code = errno;
            }
            close_pipe(p, 0);
          }
        } else {
          char buffer[8192];
          ssize_t got = read(p->fd[i], buffer, sizeof buffer);
          if (got > 0) {
            if (!add_buffer(p, i - 1, buffer, (size_t)got)) {
              failed = 1;
              code = ENOMEM;
            }
          } else if (!got || (got < 0 && errno != EINTR && errno != EAGAIN)) {
            if (got < 0) {
              failed = 1;
              code = errno;
            }
            close_pipe(p, i);
          }
        }
      }
  }
  if (!had_pipe) {
    struct timespec zero = {0};
    while (sigtimedwait(&blocked, NULL, &zero) >= 0) {
    }
  }
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  ca_thread_acquire(rt);
  if (failed)
    return ca_native_oserror(rt, code, NULL);
  CaValue roots[] = {v, CA_NONE, CA_NONE, CA_UNBOUND};
  CaFrame f;
  ca_push(rt, &f, roots, 4);
  for (int i = 0; i < 2; i++) {
    CaValue enabled = ca_dep_get(v, i ? "_stderr_pipe" : "_stdout_pipe");
    if (enabled.tag == CA_BOOL_T && enabled.as.i) {
      roots[i + 1] = ca_bytes(rt, p->buffer[i], p->used[i]);
      if (p->text && !timed) {
        CaValue args[] = {roots[i + 1], ca_string(rt, "utf-8", 5),
                          ca_string(rt, "strict", 6)};
        roots[i + 1] = ca_text_call(rt, N_TEXT_DECODE, 3, 0, args, NULL);
        if (!rt->error) {
          CaValue replace[] = {roots[i + 1], ca_string(rt, "\r\n", 2),
                               ca_string(rt, "\n", 1)};
          roots[i + 1] = ca_text_call(rt, N_TEXT_REPLACE, 3, 0, replace, NULL);
          replace[0] = roots[i + 1];
          replace[1] = ca_string(rt, "\r", 1);
          roots[i + 1] = ca_text_call(rt, N_TEXT_REPLACE, 3, 0, replace, NULL);
        }
      }
    }
  }
  if (!rt->error && timed) {
    CaValue args[] = {ca_dep_get(v, "args"), timeout, roots[1], roots[2]};
    roots[3] = ca_exception_new(rt, ca_type(CA_T_TIMEOUTEXPIRED), 4, args);
    if (!rt->error)
      ca_raise(rt, roots[3], CA_NONE, 0);
  } else if (!rt->error)
    roots[3] = ca_container(rt, CA_TUPLE_T, 2, roots + 1);
  CaValue result = roots[3];
  ca_pop(rt, &f);
  return rt->error ? CA_UNBOUND : result;
}
static CaValue check_result(CaRuntime *rt, CaValue v) {
  CaValue code = ca_dep_get(v, "returncode");
  if (code.tag == CA_INT_T && code.as.i) {
    CaValue args[] = {code, ca_dep_get(v, "args"), ca_dep_get(v, "stdout"),
                      ca_dep_get(v, "stderr")};
    CaValue e = ca_exception_new(rt, ca_type(CA_T_CALLEDPROCESSERROR), 4, args);
    if (!rt->error)
      ca_raise(rt, e, CA_NONE, 0);
  }
  return rt->error ? CA_UNBOUND : CA_NONE;
}
CaValue ca_process_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                        const char **k) {
  if (id == D_PROCESS_RUN || id == D_PROCESS_POPEN ||
      id == D_PROCESS_CHECK_CALL || id == D_PROCESS_CHECK_OUTPUT) {
    int run = id != D_PROCESS_POPEN;
    CaValue check = CA_NONE, timeout = CA_NONE, input = CA_NONE;
    CaValue *args = a;
    const char **keys = k;
    CaValue *owned = NULL;
    const char **ownedkeys = NULL;
    if (id == D_PROCESS_CHECK_OUTPUT) {
      owned = calloc((size_t)n + nkw + 1, sizeof(CaValue));
      ownedkeys = calloc((size_t)nkw + 1, sizeof(char *));
      if (!owned || !ownedkeys) {
        free(owned);
        free(ownedkeys);
        return ca_dep_error(rt, "MemoryError",
                            "process arguments allocation failed");
      }
      memcpy(owned, a, ((size_t)n + nkw) * sizeof(CaValue));
      memcpy(ownedkeys, k, (size_t)nkw * sizeof(char *));
      owned[n + nkw] = ca_int(-1);
      ownedkeys[nkw] = "stdout";
      args = owned;
      keys = ownedkeys;
      nkw++;
    }
    CaValue roots[] = {
        spawn(rt, n, nkw, args, keys, run, &check, &timeout, &input),
        CA_UNBOUND, CA_UNBOUND};
    free(owned);
    free(ownedkeys);
    CaFrame f;
    ca_push(rt, &f, roots, 3);
    if (rt->error)
      goto done;
    Process *p = ((CaForeign *)roots[0].as.obj)->handle;
    ca_dep_put(rt, roots[0], "_stdout_pipe", ca_bool(p->fd[1] >= 0));
    ca_dep_put(rt, roots[0], "_stderr_pipe", ca_bool(p->fd[2] >= 0));
    if (!run) {
      roots[2] = roots[0];
      goto done;
    }
    roots[1] = communicate(rt, roots[0], input, timeout, 1);
    if (rt->error)
      goto done;
    roots[2] = ca_dep_record(rt, "CompletedProcess");
    ca_dep_put(rt, roots[2], "args", ca_dep_get(roots[0], "args"));
    ca_dep_put(rt, roots[2], "returncode", ca_int(p->status));
    ca_dep_put(rt, roots[2], "stdout", ca_capture(roots[1], 0));
    ca_dep_put(rt, roots[2], "stderr", ca_capture(roots[1], 1));
    ca_dep_put(rt, roots[2], "check_returncode",
               ca_native_method(rt, roots[2], D_PROCESS_CHECK_RETURN));
    if (ca_truth(rt, check) || id == D_PROCESS_CHECK_CALL ||
        id == D_PROCESS_CHECK_OUTPUT)
      check_result(rt, roots[2]);
    if (id == D_PROCESS_CHECK_CALL)
      roots[2] = ca_int(0);
    if (id == D_PROCESS_CHECK_OUTPUT)
      roots[2] = ca_dep_get(roots[2], "stdout");
  done:;
    CaValue result = roots[2];
    ca_pop(rt, &f);
    return rt->error ? CA_UNBOUND : result;
  }
  if (id == D_PROCESS_CHECK_RETURN) {
    if (n != 1 || nkw)
      goto bad;
    return check_result(rt, a[0]);
  }
  if (n < 1 || a[0].tag != CA_FOREIGN_T ||
      ((CaForeign *)a[0].as.obj)->kind != CA_F_PROCESS)
    goto bad;
  Process *p = ((CaForeign *)a[0].as.obj)->handle;
  if (id == D_PROCESS_COMMUNICATE) {
    CaValue out[3], d[] = {CA_NONE, CA_NONE, CA_NONE};
    const char *names[] = {"self", "input", "timeout"};
    if (!ca_native_arguments(rt, n, nkw, a, k, 3, 1, names, d, out))
      return CA_UNBOUND;
    return communicate(rt, a[0], out[1], out[2], 0);
  }
  if (id == D_PROCESS_WAIT) {
    CaValue out[2], d[] = {CA_NONE, CA_NONE};
    const char *names[] = {"self", "timeout"};
    if (!ca_native_arguments(rt, n, nkw, a, k, 2, 1, names, d, out))
      return CA_UNBOUND;
    double timeout = out[1].tag == CA_NONE_T    ? -1
                     : out[1].tag == CA_INT_T   ? (double)out[1].as.i
                     : out[1].tag == CA_FLOAT_T ? out[1].as.f
                                                : -2;
    if (timeout == -2)
      return ca_dep_error(rt, "TypeError", "timeout must be numeric");
    double deadline = clock_now() + timeout;
    ca_thread_release(rt);
    while (!p->done) {
      poll_status(p, timeout < 0);
      if (p->done || (timeout >= 0 && clock_now() >= deadline))
        break;
      struct timespec t = {0, 1000000};
      nanosleep(&t, NULL);
    }
    ca_thread_acquire(rt);
    if (!p->done) {
      CaValue args[] = {ca_dep_get(a[0], "args"), out[1]};
      CaValue e = ca_exception_new(rt, ca_type(CA_T_TIMEOUTEXPIRED), 2, args);
      ca_raise(rt, e, CA_NONE, 0);
      return CA_UNBOUND;
    }
    return ca_int(p->status);
  }
  if (n != 1 || nkw)
    goto bad;
  if (id == D_PROCESS_POLL) {
    poll_status(p, 0);
    return p->done ? ca_int(p->status) : CA_NONE;
  }
  if (id == D_PROCESS_TERMINATE || id == D_PROCESS_KILL) {
    poll_status(p, 0);
    if (!p->done &&
        kill(p->pid, id == D_PROCESS_KILL ? SIGKILL : SIGTERM) < 0 &&
        errno != ESRCH)
      return ca_native_oserror(rt, errno, NULL);
    return CA_NONE;
  }
bad:
  return ca_dep_error(rt, "TypeError", "invalid native subprocess arguments");
}
CaValue ca_process_attribute(CaRuntime *rt, CaValue v, const char *name) {
  Process *p = ((CaForeign *)v.as.obj)->handle;
  if (!strcmp(name, "returncode"))
    return p->done ? ca_int(p->status) : CA_NONE;
  CaValue value = ca_dep_get(v, name);
  if (value.tag != CA_UNBOUND_T)
    return value;
  const char *names[] = {"wait", "poll", "communicate", "terminate", "kill"};
  int ids[] = {D_PROCESS_WAIT, D_PROCESS_POLL, D_PROCESS_COMMUNICATE,
               D_PROCESS_TERMINATE, D_PROCESS_KILL};
  for (int i = 0; i < 5; i++)
    if (!strcmp(name, names[i]))
      return ca_native_method(rt, v, ids[i]);
  const char *streams[] = {"stdin", "stdout", "stderr"};
  for (int i = 0; i < 3; i++)
    if (!strcmp(name, streams[i])) {
      if (p->fd[i] < 0)
        return CA_NONE;
      int fd = p->fd[i];
      if (fd < 0)
        return ca_native_oserror(rt, errno, NULL);
      CaValue args[] = {
          ca_int(fd),
          ca_string(rt, i ? (p->text ? "r" : "rb") : (p->text ? "w" : "wb"),
                    p->text ? 1 : 2)};
      value = ca_builtin_open(rt, 2, 0, args, NULL);
      if (rt->error) {
        return CA_UNBOUND;
      }
      p->streams[i] = (CaForeign *)value.as.obj;
      ca_dep_put(rt, v, name, value);
      return value;
    }
  return ca_dep_error(rt, "AttributeError", name);
}
void ca_process_finalize(CaForeign *f) {
  if (f->kind != CA_F_PROCESS || !f->handle)
    return;
  Process *p = f->handle;
  for (int i = 0; i < 3; i++)
    if (p->fd[i] >= 0 && !p->streams[i])
      close(p->fd[i]);
  poll_status(p, 0); /* Live Popen children are allowed to outlive their
                        wrapper, as in Python. */
  free(p->buffer[0]);
  free(p->buffer[1]);
  free(p);
  f->handle = NULL;
}
