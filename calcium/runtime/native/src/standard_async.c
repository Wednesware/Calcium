#define _POSIX_C_SOURCE 200809L
#include "dependencies.h"
#include <errno.h>
#include <time.h>
/* Task state is ordinary traced CaRT storage. No C stack survives suspension.
 */
enum {
  STACK,
  WAIT,
  RESULT,
  FAILURE,
  DONE,
  CANCEL,
  WAKE,
  DEADLINE,
  TIMINGOUT,
  STATE_COUNT
};
typedef struct {
  CaValue tasks, current, identity;
} Loop;
static CaValue *state(CaValue v) {
  return ((CaContainer *)((CaForeign *)v.as.obj)->state.as.obj)->items;
}
static int foreign(CaValue v, int kind) {
  return v.tag == CA_FOREIGN_T && ((CaForeign *)v.as.obj)->kind == kind;
}
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static CaValue failure(CaRuntime *rt, int type, const char *text) {
  CaValue v = ca_string(rt, text, strlen(text));
  return rt->error ? CA_UNBOUND
                   : ca_exception_new(rt, ca_type(type), text[0] ? 1 : 0, &v);
}
static int is_coro(CaValue value) {
  return value.tag == CA_GENERATOR_T &&
         ((CaGenerator *)value.as.obj)->coroutine;
}
static CaValue task_new(CaRuntime *rt, CaValue coro) {
  if (!is_coro(coro) && !foreign(coro, CA_F_SLEEP)) {
    ca_error(rt, "TypeError", "a native coroutine is required");
    return CA_UNBOUND;
  }
  Loop *loop = rt->event_loop;
  if (!loop) {
    ca_error(rt, "RuntimeError", "no running event loop");
    return CA_UNBOUND;
  }
  if (is_coro(coro) && (((CaGenerator *)coro.as.obj)->closed ||
                        ((CaGenerator *)coro.as.obj)->running ||
                        ((CaGenerator *)coro.as.obj)->claimed)) {
    ca_error(rt, "RuntimeError", "cannot reuse an already awaited coroutine");
    return CA_UNBOUND;
  }
  CaValue roots[2] = {coro, CA_UNBOUND};
  CaFrame f;
  ca_push(rt, &f, roots, 2);
  roots[1] = ca_container(rt, CA_LIST_T, 1, &roots[0]);
  CaValue values[STATE_COUNT] = {roots[1],    CA_NONE,    CA_NONE,
                                 CA_NONE,     ca_bool(0), ca_bool(0),
                                 ca_float(0), CA_NONE,    ca_bool(0)};
  if (!rt->error)
    roots[1] = ca_container(rt, CA_LIST_T, STATE_COUNT, values);
  CaValue task = rt->error ? CA_UNBOUND
                           : ca_native_object(rt, CA_F_TASK, roots[1], NULL, 0);
  if (!rt->error) {
    if (is_coro(coro))
      ((CaGenerator *)coro.as.obj)->claimed = 1;
    ca_append(rt, loop->tasks, task);
  }
  ca_pop(rt, &f);
  return rt->error ? CA_UNBOUND : task;
}
static void cancel_task(CaValue task) {
  if (foreign(task, CA_F_TASK) && !state(task)[DONE].as.i)
    state(task)[CANCEL] = ca_bool(1);
}
static void remove_waiter(CaValue lock, CaValue task) {
  CaContainer *q = (CaContainer *)state(lock)[1].as.obj;
  for (size_t i = 0; i < q->length; i++)
    if (q->items[i].as.obj == task.as.obj) {
      memmove(q->items + i, q->items + i + 1,
              (q->length - i - 1) * sizeof(CaValue));
      q->items[--q->length] = CA_UNBOUND;
      break;
    }
}
static int deliver_wait(CaRuntime *rt, CaValue task, CaValue *result,
                        CaValue *error) {
  CaValue *s = state(task), wait = s[WAIT];
  if (s[CANCEL].as.i == 2) {
    if (foreign(wait, CA_F_TASK) && !state(wait)[DONE].as.i)
      return 0;
    s[CANCEL] = ca_bool(0);
  }
  if (s[CANCEL].as.i) {
    if (foreign(wait, CA_F_TASK) && !state(wait)[DONE].as.i) {
      cancel_task(wait);
      s[CANCEL] = ca_int(2);
      return 0;
    }
    s[CANCEL] = ca_bool(0);
    if (foreign(wait, CA_F_LOCK_WAIT))
      remove_waiter(((CaForeign *)wait.as.obj)->state, task);
    if (foreign(wait, CA_F_GATHER)) {
      CaContainer *children = (CaContainer *)state(wait)[0].as.obj;
      for (size_t i = 0; i < children->length; i++)
        cancel_task(children->items[i]);
    }
    *error = failure(rt, CA_T_CANCELLEDERROR, "");
    return 1;
  }
  if (wait.tag == CA_NONE_T || wait.tag == CA_UNBOUND_T)
    return 1;
  if (foreign(wait, CA_F_SLEEP)) {
    if (now() < s[WAKE].as.f)
      return 0;
    *result = ca_capture(((CaForeign *)wait.as.obj)->state, 1);
    return 1;
  }
  if (foreign(wait, CA_F_TASK)) {
    CaValue *child = state(wait);
    if (!child[DONE].as.i) {
      if (s[DEADLINE].tag == CA_FLOAT_T && now() >= s[DEADLINE].as.f &&
          !s[TIMINGOUT].as.i) {
        cancel_task(wait);
        s[TIMINGOUT] = ca_bool(1);
      }
      return 0;
    }
    *result = child[RESULT];
    *error = child[FAILURE];
    if (s[TIMINGOUT].as.i && error->tag == CA_EXCEPTION_T &&
        ca_exception_match(rt, *error, ca_type(CA_T_CANCELLEDERROR))) {
      CaValue cancelled = *error;
      *error = failure(rt, CA_T_TIMEOUTERROR, "");
      if (error->tag == CA_EXCEPTION_T) {
        ((CaException *)error->as.obj)->cause = cancelled;
        ((CaException *)error->as.obj)->context = cancelled;
      }
    }
    return 1;
  }
  if (foreign(wait, CA_F_LOCK_WAIT)) {
    CaValue lock = ((CaForeign *)wait.as.obj)->state;
    CaValue *l = state(lock);
    CaContainer *q = (CaContainer *)l[1].as.obj;
    if (l[0].as.i || !q->length || q->items[0].as.obj != task.as.obj)
      return 0;
    remove_waiter(lock, task);
    l[0] = ca_bool(1);
    *result = ((CaForeign *)wait.as.obj)->auxiliary ? CA_NONE : ca_bool(1);
    return 1;
  }
  if (foreign(wait, CA_F_GATHER)) {
    CaValue *g = state(wait);
    CaContainer *children = (CaContainer *)g[0].as.obj;
    int all = 1;
    for (size_t i = 0; i < children->length; i++) {
      CaValue *child = state(children->items[i]);
      if (!child[DONE].as.i) {
        all = 0;
        continue;
      }
      if (!g[1].as.i && child[FAILURE].tag != CA_NONE_T) {
        *error = child[FAILURE];
        return 1;
      }
    }
    if (!all)
      return 0;
    *result = ca_container(rt, CA_LIST_T, 0, NULL);
    for (size_t i = 0; i < children->length && !rt->error; i++) {
      CaValue *child = state(children->items[i]);
      ca_append(rt, *result,
                child[FAILURE].tag == CA_NONE_T ? child[RESULT]
                                                : child[FAILURE]);
    }
    return 1;
  }
  ca_error(rt, "TypeError", "unsupported native awaitable");
  return -1;
}
static void task_step(CaRuntime *rt, CaValue task) {
  CaValue roots[] = {task, CA_NONE, CA_NONE, CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 4);
  CaValue *s = state(task);
  CaContainer *stack = (CaContainer *)s[STACK].as.obj;
  int ready = deliver_wait(rt, task, &roots[1], &roots[2]);
  if (!ready) {
    ca_pop(rt, &frame);
    return;
  }
  s[WAIT] = CA_NONE;
  s[DEADLINE] = CA_NONE;
  s[TIMINGOUT] = ca_bool(0);
  while (!rt->error && !s[DONE].as.i) {
    if (!stack->length) {
      s[RESULT] = roots[1];
      s[FAILURE] = roots[2];
      s[DONE] = ca_bool(1);
      break;
    }
    CaValue top = stack->items[stack->length - 1];
    int status = 1;
    if (is_coro(top)) {
      CaGenerator *g = (CaGenerator *)top.as.obj;
      if (g->closed) {
        roots[2] = failure(rt, CA_T_RUNTIMEERROR,
                           "cannot reuse an already awaited coroutine");
        status = -1;
      } else if (!g->started && roots[2].tag != CA_NONE_T) {
        g->closed = 1;
        status = -1;
      } else {
        CaValue previous = rt->handled;
        CaFrame saved;
        ca_push(rt, &saved, &previous, 1);
        rt->handled = g->handled.tag == CA_UNBOUND_T ? CA_NONE : g->handled;
        g->sent = roots[1];
        g->injected = roots[2];
        g->running = 1;
        g->started = 1;
        status = g->resume(rt, g, &roots[3]);
        g->running = 0;
        g->handled = rt->handled;
        rt->handled = previous;
        ca_pop(rt, &saved);
        if (status < 0)
          roots[2] = ca_exception_take(rt);
        else if (!status) {
          roots[1] = g->result;
          roots[2] = CA_NONE;
        }
      }
      if (status <= 0) {
        g->closed = 1;
        for (size_t i = 0; i < g->count; i++)
          g->values[i] = CA_UNBOUND;
        stack->items[--stack->length] = CA_UNBOUND;
        continue;
      }
    } else if (foreign(top, CA_F_SLEEP)) {
      if (((CaForeign *)top.as.obj)->auxiliary) {
        stack->items[--stack->length] = CA_UNBOUND;
        continue;
      }
      roots[3] = top;
    } else {
      ca_error(rt, "TypeError", "invalid coroutine frame");
      break;
    }
    roots[1] = roots[2] = CA_NONE;
    CaValue awaited = roots[3];
    if (is_coro(awaited)) {
      CaGenerator *child = (CaGenerator *)awaited.as.obj;
      if (child->claimed || child->closed) {
        roots[2] =
            failure(rt, CA_T_RUNTIMEERROR,
                    "coroutine is already being awaited or has finished");
        continue;
      }
      child->claimed = 1;
      ca_append(rt, s[STACK], awaited);
      continue;
    }
    if (foreign(awaited, CA_F_READY)) {
      roots[1] = ((CaForeign *)awaited.as.obj)->state;
      continue;
    }
    if (foreign(awaited, CA_F_SLEEP)) {
      CaForeign *f = (CaForeign *)awaited.as.obj;
      if (f->auxiliary) {
        roots[2] = failure(rt, CA_T_RUNTIMEERROR,
                           "cannot reuse an already awaited coroutine");
        continue;
      }
      f->auxiliary = 1;
      double delay = ca_capture(f->state, 0).as.f;
      s[WAKE] = ca_float(now() + (delay > 0 ? delay : 0));
      s[WAIT] = awaited;
      break;
    }
    if (foreign(awaited, CA_F_WAIT_FOR)) {
      CaValue *w = state(awaited);
      CaValue child = foreign(w[0], CA_F_TASK) ? w[0] : task_new(rt, w[0]);
      if (rt->error)
        break;
      s[WAIT] = child;
      if (w[1].tag != CA_NONE_T)
        s[DEADLINE] = ca_float(now() + ca_as_double(rt, w[1]));
      if (w[1].tag != CA_NONE_T && ca_as_double(rt, w[1]) <= 0) {
        cancel_task(child);
        s[TIMINGOUT] = ca_bool(1);
      }
      break;
    }
    if (foreign(awaited, CA_F_LOCK_WAIT)) {
      CaValue *lock = state(((CaForeign *)awaited.as.obj)->state);
      CaContainer *q = (CaContainer *)lock[1].as.obj;
      if (!lock[0].as.i && !q->length) {
        lock[0] = ca_bool(1);
        roots[1] =
            ((CaForeign *)awaited.as.obj)->auxiliary ? CA_NONE : ca_bool(1);
        continue;
      }
      ca_append(rt, lock[1], task);
      s[WAIT] = awaited;
      break;
    }
    if (foreign(awaited, CA_F_TASK) || foreign(awaited, CA_F_GATHER)) {
      if (awaited.as.obj == task.as.obj) {
        roots[2] =
            failure(rt, CA_T_RUNTIMEERROR, "Task cannot await on itself");
        continue;
      }
      s[WAIT] = awaited;
      int immediate = deliver_wait(rt, task, &roots[1], &roots[2]);
      if (immediate) {
        s[WAIT] = CA_NONE;
        continue;
      }
      break;
    }
    roots[2] = failure(rt, CA_T_TYPEERROR,
                       "object cannot be used in await expression");
  }
  if (rt->error) {
    s[FAILURE] = ca_exception_take(rt);
    s[DONE] = ca_bool(1);
  }
  ca_pop(rt, &frame);
}
static CaValue run(CaRuntime *rt, CaValue coro) {
  if (rt->event_loop) {
    ca_error(rt, "RuntimeError",
             "asyncio.run() cannot be called from a running event loop");
    return CA_UNBOUND;
  }
  Loop loop = {CA_UNBOUND, CA_NONE};
  CaValue roots[] = {coro, CA_UNBOUND, CA_UNBOUND, rt->handled, CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 5);
  roots[4] = ca_dep_record(rt, "NativeEventLoop");
  loop.identity = roots[4];
  ca_dep_put(rt, loop.identity, "_running", ca_bool(1));
  ca_dep_put(rt, loop.identity, "is_running",
             ca_native_method(rt, loop.identity, D_HELPERS_LOOP_RUNNING));
  ca_dep_put(rt, loop.identity, "create_task", ca_native_function(N_AS_TASK));
  ca_dep_put(rt, loop.identity, "time", ca_native_function(N_MONOTONIC));
  roots[1] = ca_container(rt, CA_LIST_T, 0, NULL);
  loop.tasks = roots[1];
  rt->event_loop = &loop;
  roots[2] = task_new(rt, coro);
  int draining = 0;
  while (!rt->error) {
    CaContainer *tasks = (CaContainer *)loop.tasks.as.obj;
    int active = 0;
    for (size_t i = 0; i < tasks->length && !rt->error; i++) {
      CaValue task = tasks->items[i];
      if (state(task)[DONE].as.i)
        continue;
      loop.current = task;
      task_step(rt, task);
      ca_safepoint(rt);
      if (!state(task)[DONE].as.i)
        active = 1;
    }
    if (rt->error)
      break;
    if (state(roots[2])[DONE].as.i && !draining) {
      draining = 1;
      for (size_t i = 0; i < tasks->length; i++)
        cancel_task(tasks->items[i]);
      active = 1;
    }
    if (draining && !active)
      break;
    /* Completed tasks are retained only by application/await edges, not by the
     * loop. */
    size_t kept = 0;
    for (size_t i = 0; i < tasks->length; i++)
      if (!state(tasks->items[i])[DONE].as.i)
        tasks->items[kept++] = tasks->items[i];
    for (size_t i = kept; i < tasks->length; i++)
      tasks->items[i] = CA_UNBOUND;
    tasks->length = kept;
    if (draining && !kept)
      break;
    double delay = 0.01, current = now();
    for (size_t i = 0; i < tasks->length; i++) {
      CaValue *pending = state(tasks->items[i]), wait = pending[WAIT];
      if (pending[CANCEL].as.i == 1 || wait.tag == CA_NONE_T) {
        delay = 0;
        break;
      }
      if (foreign(wait, CA_F_SLEEP))
        delay = fmin(delay, fmax(0, pending[WAKE].as.f - current));
      if (pending[DEADLINE].tag == CA_FLOAT_T && !pending[TIMINGOUT].as.i)
        delay = fmin(delay, fmax(0, pending[DEADLINE].as.f - current));
      if (foreign(wait, CA_F_TASK) && state(wait)[DONE].as.i) {
        delay = 0;
        break;
      }
      if (foreign(wait, CA_F_LOCK_WAIT) &&
          !state(((CaForeign *)wait.as.obj)->state)[0].as.i) {
        delay = 0;
        break;
      }
      if (foreign(wait, CA_F_GATHER)) {
        CaContainer *children = (CaContainer *)state(wait)[0].as.obj;
        int all = 1;
        for (size_t j = 0; j < children->length; j++)
          if (!state(children->items[j])[DONE].as.i) {
            all = 0;
            break;
          }
        if (all) {
          delay = 0;
          break;
        }
      }
    }
    if (delay > 0) {
      struct timespec pause = {0, (long)(delay * 1e9)};
      while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {
      }
    }
  }
  ca_dep_put(rt, loop.identity, "_running", ca_bool(0));
  rt->event_loop = NULL;
  rt->handled = roots[3];
  CaValue result = CA_UNBOUND;
  if (!rt->error) {
    CaValue *s = state(roots[2]);
    result = s[RESULT];
    if (s[FAILURE].tag != CA_NONE_T)
      ca_error_set(rt, s[FAILURE]);
  }
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : result;
}
CaValue ca_async_call(CaRuntime *rt, int id, int n, int nkw, CaValue *args,
                      const char **keys) {
  CaValue out[3], defaults[] = {CA_NONE, CA_NONE, CA_NONE};
  const char *names[3] = {"self", "value", "exception"};
  if (id == N_AS_RUN) {
    if (n != 1 || nkw)
      goto bad;
    return run(rt, args[0]);
  }
  if (id == N_AS_TASK) {
    if (n != 1 || nkw)
      goto bad;
    return task_new(rt, args[0]);
  }
  if (id == N_AS_SLEEP) {
    names[0] = "delay";
    names[1] = "result";
    if (!ca_native_arguments(rt, n, nkw, args, keys, 2, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    double delay = ca_as_double(rt, out[0]);
    if (rt->error)
      return CA_UNBOUND;
    if (!isfinite(delay)) {
      ca_error(rt, "ValueError", "sleep delay must be finite");
      return CA_UNBOUND;
    }
    CaValue values[] = {ca_float(delay), out[1]};
    CaValue data = ca_container(rt, CA_TUPLE_T, 2, values);
    return rt->error ? CA_UNBOUND
                     : ca_native_object(rt, CA_F_SLEEP, data, NULL, 0);
  }
  if (id == N_AS_WAIT_FOR) {
    names[0] = "fut";
    names[1] = "timeout";
    if (!ca_native_arguments(rt, n, nkw, args, keys, 2, 2, names, defaults,
                             out))
      return CA_UNBOUND;
    if (out[1].tag != CA_NONE_T) {
      double t = ca_as_double(rt, out[1]);
      if (rt->error)
        return CA_UNBOUND;
      if (!isfinite(t)) {
        ca_error(rt, "ValueError", "timeout must be finite");
        return CA_UNBOUND;
      }
    }
    CaValue data = ca_container(rt, CA_LIST_T, 2, out);
    return rt->error ? CA_UNBOUND
                     : ca_native_object(rt, CA_F_WAIT_FOR, data, NULL, 0);
  }
  if (id == N_AS_GATHER) {
    if (!rt->event_loop) {
      ca_error(rt, "RuntimeError", "gather requires a running event loop");
      return CA_UNBOUND;
    }
    int errors = 0;
    if (nkw) {
      if (nkw != 1 || strcmp(keys[0], "return_exceptions"))
        goto bad;
      errors = ca_truth(rt, args[n]);
    }
    CaValue roots[] = {ca_container(rt, CA_LIST_T, 0, NULL), CA_UNBOUND};
    CaFrame f;
    ca_push(rt, &f, roots, 2);
    for (int i = 0; i < n && !rt->error; i++) {
      roots[1] = CA_UNBOUND;
      for (int j = 0; j < i; j++)
        if (args[j].tag == args[i].tag && args[j].as.obj == args[i].as.obj) {
          roots[1] = ((CaContainer *)roots[0].as.obj)->items[j];
          break;
        }
      if (roots[1].tag == CA_UNBOUND_T)
        roots[1] =
            foreign(args[i], CA_F_TASK) ? args[i] : task_new(rt, args[i]);
      if (!rt->error)
        ca_append(rt, roots[0], roots[1]);
    }
    CaValue fields[] = {roots[0], ca_bool(errors)};
    roots[1] = ca_container(rt, CA_LIST_T, 2, fields);
    CaValue v = rt->error
                    ? CA_UNBOUND
                    : ca_native_object(rt, CA_F_GATHER, roots[1], NULL, 0);
    ca_pop(rt, &f);
    return v;
  }
  if (id == N_AS_LOCK) {
    if (n || nkw)
      goto bad;
    CaValue fields[] = {ca_bool(0), ca_container(rt, CA_LIST_T, 0, NULL)};
    CaValue data = ca_container(rt, CA_LIST_T, 2, fields);
    return rt->error ? CA_UNBOUND
                     : ca_native_object(rt, CA_F_LOCK, data, NULL, 0);
  }
  if (id >= N_LOCK_ACQUIRE && id <= N_LOCK_EXIT) {
    if (nkw || n != (id == N_LOCK_EXIT ? 4 : 1) || !foreign(args[0], CA_F_LOCK))
      goto bad;
    if (id == N_LOCK_LOCKED)
      return state(args[0])[0];
    if (id == N_LOCK_ACQUIRE || id == N_LOCK_ENTER)
      return ca_native_object(rt, CA_F_LOCK_WAIT, args[0], NULL,
                              id == N_LOCK_ENTER);
    if (!state(args[0])[0].as.i) {
      ca_error(rt, "RuntimeError", "Lock is not acquired.");
      return CA_UNBOUND;
    }
    state(args[0])[0] = ca_bool(0);
    return id == N_LOCK_EXIT
               ? ca_native_object(rt, CA_F_READY, CA_NONE, NULL, 0)
               : CA_NONE;
  }
  if (id >= N_TASK_CANCEL && id <= N_TASK_EXCEPTION) {
    if (n != 1 || nkw || !foreign(args[0], CA_F_TASK))
      goto bad;
    CaValue *s = state(args[0]);
    if (id == N_TASK_DONE)
      return s[DONE];
    if (id == N_TASK_CANCEL) {
      int active = !s[DONE].as.i;
      cancel_task(args[0]);
      return ca_bool(active);
    }
    if (!s[DONE].as.i) {
      ca_error(rt, "RuntimeError", "Result is not set.");
      return CA_UNBOUND;
    }
    if (id == N_TASK_EXCEPTION) {
      if (s[FAILURE].tag == CA_EXCEPTION_T &&
          ca_exception_match(rt, s[FAILURE], ca_type(CA_T_CANCELLEDERROR))) {
        ca_error_set(rt, s[FAILURE]);
        return CA_UNBOUND;
      }
      return s[FAILURE];
    }
    if (s[FAILURE].tag != CA_NONE_T) {
      ca_error_set(rt, s[FAILURE]);
      return CA_UNBOUND;
    }
    return s[RESULT];
  }
bad:
  ca_error(rt, "TypeError", "invalid native asyncio arguments");
  return CA_UNBOUND;
}
CaValue ca_async_attribute(CaRuntime *rt, CaValue value, const char *name) {
  int id = 0;
  if (foreign(value, CA_F_LOCK))
    id = !strcmp(name, "acquire")      ? N_LOCK_ACQUIRE
         : !strcmp(name, "release")    ? N_LOCK_RELEASE
         : !strcmp(name, "locked")     ? N_LOCK_LOCKED
         : !strcmp(name, "__aenter__") ? N_LOCK_ENTER
         : !strcmp(name, "__aexit__")  ? N_LOCK_EXIT
                                       : 0;
  if (foreign(value, CA_F_TASK))
    id = !strcmp(name, "cancel")      ? N_TASK_CANCEL
         : !strcmp(name, "done")      ? N_TASK_DONE
         : !strcmp(name, "result")    ? N_TASK_RESULT
         : !strcmp(name, "exception") ? N_TASK_EXCEPTION
                                      : 0;
  if (id)
    return ca_native_method(rt, value, id);
  ca_error(rt, "AttributeError", name);
  return CA_UNBOUND;
}

CaValue ca_async_running_loop(CaRuntime *rt) {
  if (!rt->event_loop) {
    ca_error(rt, "RuntimeError", "no running event loop");
    return CA_UNBOUND;
  }
  return ((Loop *)rt->event_loop)->identity;
}
