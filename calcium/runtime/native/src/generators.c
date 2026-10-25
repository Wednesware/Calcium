#include "standard.h"
CaValue ca_generator_new(CaRuntime *rt, CaResume resume,
                         const CaFunction *definition, size_t count) {
  if (count > (SIZE_MAX - sizeof(CaGenerator)) / sizeof(CaValue)) {
    ca_error(rt, "MemoryError", "generator frame too large");
    return CA_UNBOUND;
  }
  CaGenerator *g =
      ca_allocate(rt, sizeof *g + count * sizeof(CaValue), CA_GENERATOR_T);
  if (!g)
    return CA_UNBOUND;
  g->count = count;
  g->resume = resume;
  g->definition = definition;
  g->result = CA_NONE;
  g->sent = CA_NONE;
  return (CaValue){.tag = CA_GENERATOR_T, .as.obj = (CaObj *)g};
}
int ca_generator_next(CaRuntime *rt, CaValue value, CaValue *out) {
  CaGenerator *g = (CaGenerator *)value.as.obj;
  if (g->closed) {
    *out = CA_UNBOUND;
    return 0;
  }
  if (g->running) {
    ca_error(rt, "ValueError", "generator already executing");
    return -1;
  }
  CaFrame frame;
  ca_push(rt, &frame, &value, 1);
  g->running = 1;
  g->started = 1;
  int status = g->resume(rt, g, out);
  g->running = 0;
  g->sent = CA_NONE;
  if (status < 0 && rt->error &&
      ca_exception_match(rt, rt->pending, ca_type(CA_T_STOPITERATION))) {
    CaValue cause = ca_exception_take(rt);
    ca_error(rt, "RuntimeError", "generator raised StopIteration");
    if (rt->pending.tag == CA_EXCEPTION_T) {
      ((CaException *)rt->pending.as.obj)->cause = cause;
      ((CaException *)rt->pending.as.obj)->context = cause;
    }
  }
  if (status <= 0) {
    g->closed = 1;
    for (size_t i = 0; i < g->count; i++)
      g->values[i] = CA_UNBOUND;
  }
  ca_pop(rt, &frame);
  return status;
}
CaValue ca_generator_call(CaRuntime *rt, int kind, int n, int nkw,
                          CaValue *args, const char **keys) {
  (void)keys;
  if (nkw || n != (kind == N_GEN_CLOSE ? 1 : 2) ||
      args[0].tag != CA_GENERATOR_T) {
    ca_error(rt, "TypeError", "invalid generator method arguments");
    return CA_UNBOUND;
  }
  CaGenerator *g = (CaGenerator *)args[0].as.obj;
  if (g->running) {
    ca_error(rt, "ValueError", "generator already executing");
    return CA_UNBOUND;
  }
  if (kind == N_GEN_CLOSE || kind == N_GEN_THROW) {
    g->closed = 1;
    for (size_t i = 0; i < g->count; i++)
      g->values[i] = CA_UNBOUND;
    if (kind == N_GEN_THROW) {
      ca_raise(rt, args[1], CA_NONE, 0);
      return CA_UNBOUND;
    }
    return CA_NONE;
  }
  if (!g->started && args[1].tag != CA_NONE_T) {
    ca_error(rt, "TypeError",
             "can't send non-None value to a just-started generator");
    return CA_UNBOUND;
  }
  g->sent = args[1];
  CaValue result = CA_UNBOUND;
  int status = ca_generator_next(rt, args[0], &result);
  if (status < 0)
    return CA_UNBOUND;
  if (status)
    return result;
  ca_raise(rt, ca_exception_new(rt, ca_type(CA_T_STOPITERATION), 0, NULL),
           CA_NONE, 0);
  return CA_UNBOUND;
}
