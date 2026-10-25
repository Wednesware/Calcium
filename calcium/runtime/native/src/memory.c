#include "cart.h"
void ca_error(CaRuntime *rt, const char *kind, const char *message) {
  if (!rt->error) {
    rt->error = kind;
    snprintf(rt->message, sizeof rt->message, "%s", message);
  }
}
void *ca_allocate(CaRuntime *rt, size_t size, CaTag kind) {
  if (size > SIZE_MAX - rt->bytes) {
    ca_error(rt, "MemoryError", "allocation size overflow");
    return NULL;
  }
  CaObj *obj = calloc(1, size);
  if (!obj) {
    ca_error(rt, "MemoryError", "native heap allocation failed");
    return NULL;
  }
  obj->kind = kind;
  obj->bytes = size;
  obj->next = rt->heap;
  rt->heap = obj;
  rt->bytes += size;
  rt->allocations++;
  if (rt->bytes > rt->peak_bytes)
    rt->peak_bytes = rt->bytes;
  return obj;
}
void ca_push(CaRuntime *rt, CaFrame *frame, CaValue *v, size_t n) {
  frame->values = v;
  frame->count = n;
  frame->previous = rt->roots;
  rt->roots = frame;
}
void ca_pop(CaRuntime *rt, CaFrame *frame) { rt->roots = frame->previous; }
static void mark(CaValue value) {
  if (value.tag != CA_BIG_T && value.tag != CA_STR_T &&
      value.tag != CA_RANGE_T && value.tag != CA_ITER_T)
    return;
  CaObj *obj = value.as.obj;
  if (!obj || obj->marked)
    return;
  obj->marked = 1;
  if (obj->kind == CA_RANGE_T) {
    CaRange *r = (CaRange *)obj;
    mark(r->start);
    mark(r->stop);
    mark(r->step);
  }
  if (obj->kind == CA_ITER_T) {
    CaIter *i = (CaIter *)obj;
    mark(i->iterable);
    mark(i->current);
  }
}
void ca_safepoint(CaRuntime *rt) {
  if (rt->bytes < (rt->threshold ? rt->threshold : 65536))
    return;
  for (CaFrame *f = rt->roots; f; f = f->previous)
    for (size_t i = 0; i < f->count; i++)
      mark(f->values[i]);
  CaObj **link = &rt->heap;
  while (*link) {
    CaObj *o = *link;
    if (o->marked) {
      o->marked = 0;
      link = &o->next;
    } else {
      *link = o->next;
      rt->bytes -= o->bytes;
      free(o);
    }
  }
  rt->threshold =
      rt->bytes > (SIZE_MAX - 65536) / 2 ? SIZE_MAX : rt->bytes * 2 + 65536;
  rt->collections++;
}
void ca_destroy(CaRuntime *rt) {
  while (rt->heap) {
    CaObj *next = rt->heap->next;
    free(rt->heap);
    rt->heap = next;
  }
  rt->bytes = 0;
}
int ca_enter(CaRuntime *rt) {
  if (rt->depth >= 256) {
    ca_error(rt, "RecursionError", "native recursion limit (256) exceeded");
    return 0;
  }
  rt->depth++;
  return 1;
}
void ca_leave(CaRuntime *rt) { rt->depth--; }
