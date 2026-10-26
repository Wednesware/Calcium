#define _GNU_SOURCE
#include "dependencies.h"
#ifdef CA_WITH_THREADS
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
typedef struct Context {
  struct Context *next;
  CaRuntime *rt;
  CaFrame *roots;
  CaValue environment, pending, handled, value;
  void *loop;
  const char *error;
  char message[256];
  unsigned depth;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  atomic_int started, done;
  atomic_int joined;
  int daemon;
} Context;
typedef struct {
  pthread_mutex_t gil;
  Context *contexts;
  Context main;
} Threads;
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  int held, event, reentrant, depth;
  pthread_t owner;
} Lock;
static _Thread_local Context *current;
static void save(CaRuntime *rt) {
  if (!current)
    return;
  current->roots = rt->roots;
  current->environment = rt->environment;
  current->pending = rt->pending;
  current->handled = rt->handled;
  current->loop = rt->event_loop;
  current->error = rt->error;
  current->depth = rt->depth;
  memcpy(current->message, rt->message, sizeof rt->message);
}
static void restore(CaRuntime *rt) {
  rt->roots = current->roots;
  rt->environment = current->environment;
  rt->pending = current->pending;
  rt->handled = current->handled;
  rt->event_loop = current->loop;
  rt->error = current->error;
  rt->depth = current->depth;
  memcpy(rt->message, current->message, sizeof rt->message);
}
static Threads *setup(CaRuntime *rt) {
  if (rt->threading)
    return rt->threading;
  Threads *t = calloc(1, sizeof *t);
  if (!t) {
    ca_error(rt, "MemoryError", "thread runtime allocation failed");
    return NULL;
  }
  pthread_mutex_init(&t->gil, NULL);
  pthread_mutex_lock(&t->gil);
  current = &t->main;
  current->rt = rt;
  current->thread = pthread_self();
  atomic_init(&current->started, 1);
  atomic_init(&current->done, 0);
  t->contexts = &t->main;
  rt->threading = t;
  return t;
}
void ca_thread_release(CaRuntime *rt) {
  if (!rt->threading)
    return;
  save(rt);
  pthread_mutex_unlock(&((Threads *)rt->threading)->gil);
}
void ca_thread_acquire(CaRuntime *rt) {
  if (!rt->threading)
    return;
  pthread_mutex_lock(&((Threads *)rt->threading)->gil);
  restore(rt);
}
void ca_thread_yield(CaRuntime *rt) {
  if (!rt->threading ||
      ((Threads *)rt->threading)->contexts == &((Threads *)rt->threading)->main)
    return;
  ca_thread_release(rt);
  sched_yield();
  ca_thread_acquire(rt);
}
void ca_thread_roots(CaRuntime *rt, void (*mark)(CaValue, CaObj **),
                     CaObj **pending) {
  if (!rt->threading)
    return;
  for (Context *c = ((Threads *)rt->threading)->contexts; c; c = c->next) {
    if (c == &((Threads *)rt->threading)->main ||
        (atomic_load(&c->started) && !atomic_load(&c->done)))
      mark(c->value, pending);
    if (c == current)
      continue;
    mark(c->environment, pending);
    mark(c->pending, pending);
    mark(c->handled, pending);
    for (CaFrame *f = c->roots; f; f = f->previous)
      for (size_t i = 0; i < f->count; i++)
        mark(f->values[i], pending);
  }
}
static void *worker(void *value) {
  Context *ctx = value;
  current = ctx;
  CaRuntime *rt = ctx->rt;
  ca_thread_acquire(rt);
  CaValue roots[] = {ctx->value, ca_dep_get(ctx->value, "_target"),
                     ca_dep_get(ctx->value, "_args"),
                     ca_dep_get(ctx->value, "_kwargs")};
  CaFrame f;
  ca_push(rt, &f, roots, 4);
  if (roots[1].tag != CA_NONE_T)
    ca_call_ex(rt, roots[1], roots[2], roots[3]);
  if (rt->error) {
    CaValue error = ca_exception_take(rt);
    ca_dep_put(rt, ctx->value, "_exception", error);
    CaValue text = ca_stringify(rt, error, 0);
    if (!rt->error)
      fprintf(stderr, "Exception in native thread: %s: %s\n",
              ca_type_name(((CaException *)error.as.obj)->type),
              ((CaString *)text.as.obj)->data);
    if (rt->error)
      ca_exception_take(rt);
  }
  ca_dep_put(rt, ctx->value, "_target", CA_NONE);
  ca_dep_put(rt, ctx->value, "_args", CA_NONE);
  ca_dep_put(rt, ctx->value, "_kwargs", CA_NONE);
  ca_dep_put(rt, ctx->value, "_exception", CA_NONE);
  ca_pop(rt, &f);
  ctx->roots = NULL;
  ctx->environment = ctx->pending = ctx->handled = CA_NONE;
  ctx->loop = NULL;
  pthread_mutex_lock(&ctx->mutex);
  atomic_store(&ctx->done, 1);
  pthread_cond_broadcast(&ctx->condition);
  pthread_mutex_unlock(&ctx->mutex);
  /* Save the empty worker context before releasing the runtime lock. */
  rt->roots = NULL;
  rt->environment = rt->handled = rt->pending = CA_NONE;
  rt->event_loop = NULL;
  rt->error = NULL;
  ca_thread_release(rt);
  return NULL;
}
static int seconds(CaRuntime *rt, CaValue value, double *out) {
  if (value.tag == CA_NONE_T) {
    *out = -1;
    return 1;
  }
  if (value.tag == CA_INT_T)
    *out = (double)value.as.i;
  else if (value.tag == CA_FLOAT_T)
    *out = value.as.f;
  else {
    ca_error(rt, "TypeError", "timeout must be numeric");
    return 0;
  }
  if (!isfinite(*out) || *out > 1e9) {
    ca_error(rt, "OverflowError", "thread timeout outside supported range");
    return 0;
  }
  return 1;
}
static struct timespec deadline(double seconds) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  long nano = (long)((seconds - floor(seconds)) * 1e9);
  t.tv_sec += (time_t)floor(seconds);
  t.tv_nsec += nano;
  if (t.tv_nsec >= 1000000000) {
    t.tv_sec++;
    t.tv_nsec -= 1000000000;
  }
  return t;
}
static void condition_init(pthread_cond_t *condition) {
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(condition, &attr);
  pthread_condattr_destroy(&attr);
}
static int join(CaRuntime *rt, Context *ctx, double timeout) {
  if (ctx->joined)
    return 1;
  struct timespec until = deadline(timeout > 0 ? timeout : 0);
  ca_thread_release(rt);
  pthread_mutex_lock(&ctx->mutex);
  int code = 0;
  while (!atomic_load(&ctx->done) && !code)
    code = timeout < 0
               ? pthread_cond_wait(&ctx->condition, &ctx->mutex)
               : pthread_cond_timedwait(&ctx->condition, &ctx->mutex, &until);
  int done = atomic_load(&ctx->done);
  pthread_mutex_unlock(&ctx->mutex);
  if (done && !atomic_exchange(&ctx->joined, 1))
    pthread_join(ctx->thread, NULL);
  ca_thread_acquire(rt);
  return done;
}
CaValue ca_thread_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                       const char **k) {
  Threads *threads = setup(rt);
  if (!threads)
    return CA_UNBOUND;
  if (id == D_THREAD_THREAD) {
    CaValue out[6],
        d[] = {CA_NONE, CA_NONE, CA_NONE, CA_NONE, CA_NONE, CA_NONE};
    const char *names[] = {"group", "target", "name",
                           "args",  "kwargs", "daemon"};
    if (!ca_native_arguments(rt, n, nkw, a, k, 6, 0, names, d, out))
      return CA_UNBOUND;
    if (out[0].tag != CA_NONE_T)
      return ca_dep_error(rt, "RuntimeError", "thread group must be None");
    Context *ctx = calloc(1, sizeof *ctx);
    if (!ctx)
      return ca_dep_error(rt, "MemoryError", "thread allocation failed");
    ctx->rt = rt;
    pthread_mutex_init(&ctx->mutex, NULL);
    condition_init(&ctx->condition);
    atomic_init(&ctx->started, 0);
    atomic_init(&ctx->done, 0);
    CaValue state = ca_container(rt, CA_DICT_T, 0, NULL),
            v = ca_native_object(rt, CA_F_THREAD, state, ctx, 0);
    if (rt->error) {
      pthread_mutex_destroy(&ctx->mutex);
      pthread_cond_destroy(&ctx->condition);
      free(ctx);
      return CA_UNBOUND;
    }
    ctx->value = v;
    ctx->next = threads->contexts;
    threads->contexts = ctx;
    ca_dep_put(rt, v, "_target", out[1]);
    ca_dep_put(rt, v, "name",
               out[2].tag == CA_NONE_T ? ca_string(rt, "Thread", 6) : out[2]);
    ca_dep_put(rt, v, "_args",
               out[3].tag == CA_NONE_T ? ca_container(rt, CA_TUPLE_T, 0, NULL)
                                       : out[3]);
    ca_dep_put(rt, v, "_kwargs",
               out[4].tag == CA_NONE_T ? ca_container(rt, CA_DICT_T, 0, NULL)
                                       : out[4]);
    ca_dep_put(rt, v, "daemon",
               out[5].tag == CA_NONE_T ? ca_bool(current->daemon)
                                       : ca_bool(ca_truth(rt, out[5])));
    ca_dep_put(rt, v, "_started", ca_bool(0));
    return v;
  }
  if (id == D_THREAD_LOCK || id == D_THREAD_RLOCK || id == D_THREAD_EVENT) {
    if (n || nkw)
      goto bad;
    Lock *lock = calloc(1, sizeof *lock);
    if (!lock)
      return ca_dep_error(rt, "MemoryError", "lock allocation failed");
    pthread_mutex_init(&lock->mutex, NULL);
    condition_init(&lock->condition);
    lock->event = id == D_THREAD_EVENT;
    lock->reentrant = id == D_THREAD_RLOCK;
    CaValue v =
        ca_native_object(rt, lock->event ? CA_F_EVENT : CA_F_THREAD_LOCK,
                         ca_container(rt, CA_DICT_T, 0, NULL), lock, 0);
    if (rt->error) {
      pthread_mutex_destroy(&lock->mutex);
      pthread_cond_destroy(&lock->condition);
      free(lock);
    }
    return v;
  }
  if (id == D_THREAD_IDENT) {
    if (n || nkw)
      goto bad;
    return ca_int((int64_t)(uintptr_t)pthread_self());
  }
  if (id == D_THREAD_ACTIVE) {
    if (n || nkw)
      goto bad;
    int count = 1;
    for (Context *c = threads->contexts; c; c = c->next)
      if (c != &threads->main && atomic_load(&c->started) &&
          !atomic_load(&c->done))
        count++;
    return ca_int(count);
  }
  if (id == D_THREAD_CURRENT) {
    if (n || nkw)
      goto bad;
    if (current->value.tag == CA_UNBOUND_T) {
      CaValue v = ca_dep_record(rt, "MainThread");
      ca_dep_put(rt, v, "name", ca_string(rt, "MainThread", 10));
      ca_dep_put(rt, v, "daemon", ca_bool(0));
      ca_dep_put(rt, v, "ident", ca_int((int64_t)(uintptr_t)pthread_self()));
      current->value = v;
    }
    return current->value;
  }
  if (n < 1 || a[0].tag != CA_FOREIGN_T)
    goto bad;
  CaForeign *f = (CaForeign *)a[0].as.obj;
  if (f->kind == CA_F_THREAD) {
    Context *ctx = f->handle;
    if (id == D_THREAD_START) {
      if (n != 1 || nkw)
        goto bad;
      if (atomic_load(&ctx->started))
        return ca_dep_error(rt, "RuntimeError",
                            "threads can only be started once");
      ctx->daemon = ca_truth(rt, ca_dep_get(a[0], "daemon"));
      atomic_store(&ctx->started, 1);
      int err = pthread_create(&ctx->thread, NULL, worker, ctx);
      if (err) {
        atomic_store(&ctx->started, 0);
        return ca_native_oserror(rt, err, NULL);
      }
      ca_dep_put(rt, a[0], "_started", ca_bool(1));
      return CA_NONE;
    }
    if (id == D_THREAD_JOIN) {
      CaValue out[2], d[] = {CA_NONE, CA_NONE};
      const char *names[] = {"self", "timeout"};
      if (!ca_native_arguments(rt, n, nkw, a, k, 2, 1, names, d, out))
        return CA_UNBOUND;
      if (!atomic_load(&ctx->started))
        return ca_dep_error(rt, "RuntimeError",
                            "cannot join thread before it is started");
      if (ctx == current)
        return ca_dep_error(rt, "RuntimeError", "cannot join current thread");
      double timeout;
      if (!seconds(rt, out[1], &timeout))
        return CA_UNBOUND;
      if (timeout < 0 && out[1].tag != CA_NONE_T)
        timeout = 0;
      join(rt, ctx, timeout);
      return CA_NONE;
    }
    if (id == D_THREAD_ALIVE) {
      if (n != 1 || nkw)
        goto bad;
      return ca_bool(atomic_load(&ctx->started) && !atomic_load(&ctx->done));
    }
    goto bad;
  }
  if (f->kind != CA_F_THREAD_LOCK && f->kind != CA_F_EVENT)
    goto bad;
  Lock *lock = f->handle;
  if (id == D_THREAD_ENTER) {
    if (n != 1 || nkw || lock->event)
      goto bad;
    CaValue args[] = {a[0]};
    return ca_thread_call(rt, D_THREAD_ACQUIRE, 1, 0, args, NULL);
  }
  if (id == D_THREAD_RELEASE || id == D_THREAD_EXIT) {
    if (nkw || (id == D_THREAD_RELEASE ? n != 1 : n != 4) || lock->event)
      goto bad;
    pthread_mutex_lock(&lock->mutex);
    if (!lock->held ||
        (lock->reentrant && !pthread_equal(lock->owner, pthread_self()))) {
      pthread_mutex_unlock(&lock->mutex);
      return ca_dep_error(rt, "RuntimeError", "cannot release unowned lock");
    }
    if (!lock->reentrant || !--lock->depth) {
      lock->held = 0;
      pthread_cond_signal(&lock->condition);
    }
    pthread_mutex_unlock(&lock->mutex);
    return id == D_THREAD_EXIT ? ca_bool(0) : CA_NONE;
  }
  if (id == D_THREAD_LOCKED || id == D_THREAD_IS_SET) {
    if (n != 1 || nkw)
      goto bad;
    pthread_mutex_lock(&lock->mutex);
    int held = lock->held;
    pthread_mutex_unlock(&lock->mutex);
    return ca_bool(held);
  }
  if (id == D_THREAD_SET || id == D_THREAD_CLEAR) {
    if (n != 1 || nkw || !lock->event)
      goto bad;
    pthread_mutex_lock(&lock->mutex);
    lock->held = id == D_THREAD_SET;
    if (lock->held)
      pthread_cond_broadcast(&lock->condition);
    pthread_mutex_unlock(&lock->mutex);
    return CA_NONE;
  }
  if (id == D_THREAD_ACQUIRE || id == D_THREAD_WAIT) {
    CaValue out[3], d[] = {CA_NONE, ca_bool(1), ca_int(-1)};
    const char *names[] = {"self", "blocking", "timeout"};
    int event = lock->event;
    if (event) {
      names[1] = "timeout";
      d[1] = CA_NONE;
    }
    if (!ca_native_arguments(rt, n, nkw, a, k, event ? 2 : 3, 1, names, d, out))
      return CA_UNBOUND;
    int blocking = event ? 1 : ca_truth(rt, out[1]);
    double timeout;
    if (!seconds(rt, event ? out[1] : out[2], &timeout))
      return CA_UNBOUND;
    if (!blocking && timeout != -1)
      return ca_dep_error(
          rt, "ValueError",
          "timeout cannot be specified for non-blocking acquire");
    if (!event && timeout < 0 && timeout != -1)
      return ca_dep_error(rt, "ValueError", "invalid lock timeout");
    if (event && timeout < 0 && out[1].tag != CA_NONE_T)
      timeout = 0;
    struct timespec until = deadline(timeout > 0 ? timeout : 0);
    ca_thread_release(rt);
    pthread_mutex_lock(&lock->mutex);
    int code = 0;
    if (!event && lock->reentrant && lock->held &&
        pthread_equal(lock->owner, pthread_self())) {
      lock->depth++;
      pthread_mutex_unlock(&lock->mutex);
      ca_thread_acquire(rt);
      return ca_bool(1);
    }
    while ((event ? !lock->held : lock->held) && blocking && !code)
      code = timeout < 0 ? pthread_cond_wait(&lock->condition, &lock->mutex)
                         : pthread_cond_timedwait(&lock->condition,
                                                  &lock->mutex, &until);
    int ok = event ? lock->held : !lock->held;
    if (ok && !event) {
      lock->held = 1;
      lock->owner = pthread_self();
      lock->depth = 1;
    }
    pthread_mutex_unlock(&lock->mutex);
    ca_thread_acquire(rt);
    return ca_bool(ok);
  }
bad:
  return ca_dep_error(rt, "TypeError", "invalid native threading arguments");
}
CaValue ca_thread_attribute(CaRuntime *rt, CaValue v, const char *name) {
  CaForeign *f = (CaForeign *)v.as.obj;
  if (f->kind == CA_F_THREAD) {
    Context *ctx = f->handle;
    if (!strcmp(name, "ident"))
      return atomic_load(&ctx->started)
                 ? ca_int((int64_t)(uintptr_t)ctx->thread)
                 : CA_NONE;
    CaValue own = ca_dep_get(v, name);
    if (own.tag != CA_UNBOUND_T && name[0] != '_')
      return own;
    const char *names[] = {"start", "join", "is_alive"};
    int ids[] = {D_THREAD_START, D_THREAD_JOIN, D_THREAD_ALIVE};
    for (int i = 0; i < 3; i++)
      if (!strcmp(name, names[i]))
        return ca_native_method(rt, v, ids[i]);
  } else {
    const char *names[] = {"acquire",   "release",  "locked",
                           "__enter__", "__exit__", "set",
                           "clear",     "is_set",   "wait"};
    int ids[] = {D_THREAD_ACQUIRE, D_THREAD_RELEASE, D_THREAD_LOCKED,
                 D_THREAD_ENTER,   D_THREAD_EXIT,    D_THREAD_SET,
                 D_THREAD_CLEAR,   D_THREAD_IS_SET,  D_THREAD_WAIT};
    for (int i = f->kind == CA_F_EVENT ? 5 : 0;
         i < (f->kind == CA_F_EVENT ? 9 : 5); i++)
      if (!strcmp(name, names[i]))
        return ca_native_method(rt, v, ids[i]);
  }
  return ca_dep_error(rt, "AttributeError", name);
}
int ca_thread_shutdown(CaRuntime *rt) {
  if (!rt->threading)
    return 0;
  Threads *t = rt->threading;
  for (;;) {
    Context *target = NULL;
    for (Context *c = t->contexts; c; c = c->next)
      if (c != &t->main && atomic_load(&c->started) && !c->joined &&
          (!c->daemon || atomic_load(&c->done))) {
        target = c;
        break;
      }
    if (!target)
      break;
    join(rt, target, -1);
  }
  int daemon = 0;
  for (Context *c = t->contexts; c; c = c->next)
    if (c != &t->main && atomic_load(&c->started) && !c->joined) {
      pthread_detach(c->thread);
      daemon = 1;
    }
  /* A live daemon may be sleeping in native code. Retain the runtime until the
     immediately following process exit, avoiding teardown beneath that worker.
   */
  if (daemon)
    return 1;
  Context *c = t->contexts;
  while (c) {
    Context *next = c->next;
    if (c != &t->main) {
      if (c->value.tag == CA_FOREIGN_T)
        ((CaForeign *)c->value.as.obj)->handle = NULL;
      pthread_mutex_destroy(&c->mutex);
      pthread_cond_destroy(&c->condition);
      free(c);
    }
    c = next;
  }
  pthread_mutex_unlock(&t->gil);
  pthread_mutex_destroy(&t->gil);
  free(t);
  rt->threading = NULL;
  current = NULL;
  return 0;
}
void ca_thread_finalize(CaForeign *f) {
  if (f->kind == CA_F_THREAD && f->handle) {
    ((Context *)f->handle)->value = CA_UNBOUND;
    f->handle = NULL;
  }
  if ((f->kind == CA_F_THREAD_LOCK || f->kind == CA_F_EVENT) && f->handle) {
    Lock *lock = f->handle;
    pthread_mutex_destroy(&lock->mutex);
    pthread_cond_destroy(&lock->condition);
    free(lock);
    f->handle = NULL;
  }
}
#else
void ca_thread_release(CaRuntime *rt) { (void)rt; }
void ca_thread_acquire(CaRuntime *rt) { (void)rt; }
void ca_thread_yield(CaRuntime *rt) { (void)rt; }
int ca_thread_shutdown(CaRuntime *rt) {
  (void)rt;
  return 0;
}
void ca_thread_roots(CaRuntime *rt, void (*mark)(CaValue, CaObj **),
                     CaObj **p) {
  (void)rt;
  (void)mark;
  (void)p;
}
CaValue ca_thread_call(CaRuntime *rt, int id, int n, int nkw, CaValue *a,
                       const char **k) {
  (void)id;
  (void)n;
  (void)nkw;
  (void)a;
  (void)k;
  return ca_dep_error(rt, "ImportError",
                      "native threading requires POSIX thread support");
}
CaValue ca_thread_attribute(CaRuntime *rt, CaValue v, const char *name) {
  (void)v;
  return ca_dep_error(rt, "AttributeError", name);
}
void ca_thread_finalize(CaForeign *f) { (void)f; }
#endif
