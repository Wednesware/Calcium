#include "dependencies.h"
void ca_error(CaRuntime *rt, const char *kind, const char *message) {
  if (!rt->error) {
    rt->error = kind;
    snprintf(rt->message, sizeof rt->message, "%s", message);
    CaException *e = ca_allocate(rt, sizeof *e, CA_EXCEPTION_T);
    if (e) {
      e->type = ca_exception_type(kind);
      if (e->type.tag == CA_UNBOUND_T)
        e->type = ca_type(CA_T_EXCEPTION);
      e->args = CA_UNBOUND;
      e->message = ca_string(rt, message, strlen(message));
      e->cause = CA_NONE;
      e->context = rt->handled.tag == CA_EXCEPTION_T ? rt->handled : CA_NONE;
      rt->pending = (CaValue){.tag = CA_EXCEPTION_T, .as.obj = (CaObj *)e};
      e->trace = ca_trace_capture(rt);
    }
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
  frame->file = NULL;
  frame->function = NULL;
  frame->line = 1;
  frame->values = v;
  frame->count = n;
  frame->previous = rt->roots;
  rt->roots = frame;
}
void ca_pop(CaRuntime *rt, CaFrame *frame) { rt->roots = frame->previous; }
static void mark(CaValue value, CaObj **pending) {
  if (value.tag != CA_BIG_T && value.tag != CA_STR_T &&
      value.tag != CA_RANGE_T && value.tag != CA_ITER_T &&
      !ca_is_container(value) && value.tag != CA_SLICE_T &&
      value.tag != CA_METHOD_T && value.tag != CA_CELL_T &&
      value.tag != CA_CLOSURE_T && value.tag != CA_CLASS_T &&
      value.tag != CA_INSTANCE_T && value.tag != CA_BOUND_T &&
      value.tag != CA_EXCEPTION_T && value.tag != CA_BYTES_T &&
      value.tag != CA_FOREIGN_T && value.tag != CA_GENERATOR_T)
    return;
  CaObj *obj = value.as.obj;
  if (!obj || obj->marked)
    return;
  obj->marked = 1;
  obj->mark_next = *pending;
  *pending = obj;
}
void ca_safepoint(CaRuntime *rt) {
  ca_thread_yield(rt);
  if (rt->bytes < (rt->threshold ? rt->threshold : 65536))
    return;
  CaObj *pending = NULL;
  ca_thread_roots(rt, mark, &pending);
  mark(rt->pending, &pending);
  mark(rt->handled, &pending);
  mark(rt->environment, &pending);
  mark(rt->arguments, &pending);
  for (CaFrame *f = rt->roots; f; f = f->previous)
    for (size_t i = 0; i < f->count; i++)
      mark(f->values[i], &pending);
  for (size_t m = 0; m < rt->module_count; m++)
    for (CaBinding *b = rt->modules[m]->extra; b; b = b->next)
      mark(b->value, &pending);
  for (size_t m = 0; m < rt->standard_count; m++)
    for (CaBinding *b = rt->standard_modules[m]->extra; b; b = b->next)
      mark(b->value, &pending);
  while (pending) {
    CaObj *obj = pending;
    pending = obj->mark_next;
    obj->mark_next = NULL;
    CaValue value = {.tag = obj->kind, .as.obj = obj};
    if (obj->kind == CA_GENERATOR_T) {
      CaGenerator *g = (CaGenerator *)obj;
      mark(g->sent, &pending);
      mark(g->result, &pending);
      mark(g->injected, &pending);
      mark(g->handled, &pending);
      for (size_t i = 0; i < g->count; i++)
        mark(g->values[i], &pending);
    } else if (obj->kind == CA_CELL_T)
      mark(((CaCell *)obj)->value, &pending);
    else if (obj->kind == CA_CLOSURE_T) {
      CaClosure *c = (CaClosure *)obj;
      mark(c->environment, &pending);
      for (int i = 0; i < c->fn.count; i++)
        mark(c->defaults[i], &pending);
    } else if (obj->kind == CA_CLASS_T) {
      CaClass *c = (CaClass *)obj;
      mark(c->name, &pending);
      mark(c->namespace_, &pending);
      mark(c->parent, &pending);
      mark(c->fields, &pending);
    } else if (obj->kind == CA_INSTANCE_T) {
      CaInstance *i = (CaInstance *)obj;
      mark(i->type, &pending);
      mark(i->namespace_, &pending);
    } else if (obj->kind == CA_BOUND_T) {
      CaBound *b = (CaBound *)obj;
      mark(b->receiver, &pending);
      mark(b->function, &pending);
    } else if (obj->kind == CA_EXCEPTION_T) {
      CaException *e = (CaException *)obj;
      mark(e->type, &pending);
      mark(e->args, &pending);
      mark(e->cause, &pending);
      mark(e->context, &pending);
      mark(e->message, &pending);
      mark(e->attributes, &pending);
      mark(e->trace, &pending);
    } else if (obj->kind == CA_FOREIGN_T)
      mark(((CaForeign *)obj)->state, &pending);
    else if (ca_is_container(value)) {
      CaContainer *c = (CaContainer *)obj;
      for (size_t i = 0; i < c->length; i++)
        mark(c->items[i], &pending);
    } else if (obj->kind == CA_RANGE_T) {
      CaRange *r = (CaRange *)obj;
      mark(r->start, &pending);
      mark(r->stop, &pending);
      mark(r->step, &pending);
    } else if (obj->kind == CA_ITER_T) {
      CaIter *it = (CaIter *)obj;
      mark(it->iterable, &pending);
      mark(it->current, &pending);
    } else if (obj->kind == CA_METHOD_T) {
      mark(((CaMethod *)obj)->receiver, &pending);
    } else if (obj->kind == CA_SLICE_T) {
      CaSlice *s = (CaSlice *)obj;
      mark(s->start, &pending);
      mark(s->stop, &pending);
      mark(s->step, &pending);
    }
  }
  CaObj **link = &rt->heap;
  while (*link) {
    CaObj *o = *link;
    if (o->marked) {
      o->marked = 0;
      link = &o->next;
    } else {
      *link = o->next;
      rt->bytes -= o->bytes;
      if (o->kind >= CA_LIST_T && o->kind <= CA_SET_T)
        free(((CaContainer *)o)->items);
      if (o->kind == CA_FOREIGN_T)
        ca_foreign_finalize((CaForeign *)o);
      free(o);
    }
  }
  rt->threshold =
      rt->bytes > (SIZE_MAX - 65536) / 2 ? SIZE_MAX : rt->bytes * 2 + 65536;
  rt->collections++;
}
void ca_collect(CaRuntime *rt) {
  size_t previous = rt->threshold;
  rt->threshold = 1;
  ca_safepoint(rt);
  if (rt->threshold == 1)
    rt->threshold = previous;
}
void ca_destroy(CaRuntime *rt) {
  if (ca_thread_shutdown(rt))
    return;
  ca_modules_destroy(rt);
  ca_standard_destroy(rt);
  while (rt->heap) {
    CaObj *next = rt->heap->next;
    if (rt->heap->kind >= CA_LIST_T && rt->heap->kind <= CA_SET_T)
      free(((CaContainer *)rt->heap)->items);
    if (rt->heap->kind == CA_FOREIGN_T)
      ca_foreign_finalize((CaForeign *)rt->heap);
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
