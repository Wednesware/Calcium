#include "dependencies.h"
#ifdef CA_WITH_OPENSSL
#include <openssl/evp.h>
#endif
static CaValue field(CaRuntime *rt, CaValue value, CaValue factory,
                     CaValue type, CaValue name, CaValue repr,
                     CaValue compare) {
  CaValue values[] = {value, factory, type, name, repr, compare};
  CaValue state = ca_container(rt, CA_TUPLE_T, 6, values);
  return rt->error ? CA_UNBOUND
                   : ca_native_object(rt, CA_F_FIELD, state, NULL, 0);
}
static CaValue fields_mapping(CaValue type) {
  return ca_class_lookup(type, "__dataclass_fields__");
}
static CaValue field_for(CaValue type, const char *name) {
  CaValue mapping = fields_mapping(type);
  return mapping.tag == CA_DICT_T ? ca_namespace_get(mapping, name)
                                  : CA_UNBOUND;
}
int ca_dataclass_repr_field(CaValue type, const char *name) {
  CaValue f = field_for(type, name);
  return f.tag != CA_FOREIGN_T ||
         ca_capture(((CaForeign *)f.as.obj)->state, 4).as.i;
}
int ca_dataclass_equal_field(CaValue type, const char *name) {
  CaValue f = field_for(type, name);
  return f.tag != CA_FOREIGN_T ||
         ca_capture(((CaForeign *)f.as.obj)->state, 5).as.i;
}
CaValue ca_data_attribute(CaRuntime *rt, CaValue value, const char *name) {
  CaForeign *f = (CaForeign *)value.as.obj;
  if (f->kind == CA_F_FIELD) {
    const char *names[] = {"default", "default_factory", "type", "name",
                           "repr",    "compare"};
    for (size_t i = 0; i < 6; i++)
      if (!strcmp(name, names[i])) {
        CaValue v = ca_capture(f->state, i);
        if (v.tag == CA_UNBOUND_T) {
          ca_error(rt, "NotImplementedError",
                   "dataclasses.MISSING reflection is not implemented");
          return CA_UNBOUND;
        }
        return v;
      }
  }
  if (f->kind == CA_F_PARTIAL) {
    if (!strcmp(name, "func"))
      return ca_capture(f->state, 0);
    if (!strcmp(name, "args"))
      return ca_capture(f->state, 1);
    if (!strcmp(name, "keywords"))
      return ca_capture(f->state, 2);
  }
  ca_error(rt, "AttributeError", name);
  return CA_UNBOUND;
}
void ca_data_finalize(CaForeign *f) {
#ifdef CA_WITH_OPENSSL
  if (f->kind == CA_F_HASH && f->handle) {
    EVP_MD_CTX_free(f->handle);
    f->handle = NULL;
  }
#else
  (void)f;
#endif
}
static CaValue dataclass_decorate(CaRuntime *rt, CaValue value) {
  if (value.tag != CA_CLASS_T) {
    ca_error(rt, "TypeError", "dataclass expects a class");
    return CA_UNBOUND;
  }
  CaClass *cls = (CaClass *)value.as.obj;
  CaValue roots[] = {value, ca_container(rt, CA_DICT_T, 0, NULL),
                     ca_container(rt, CA_LIST_T, 0, NULL)};
  CaFrame frame;
  ca_push(rt, &frame, roots, 3);
  if (rt->error) {
    ca_pop(rt, &frame);
    return CA_UNBOUND;
  }
  if (cls->parent.tag == CA_CLASS_T) {
    CaClass *parent = (CaClass *)cls->parent.as.obj;
    if (parent->dataclass) {
      CaValue inherited = fields_mapping(cls->parent);
      if (inherited.tag == CA_DICT_T)
        ca_kwargs_merge(rt, roots[1], inherited, 0);
      if (parent->fields.tag == CA_LIST_T)
        ca_extend(rt, roots[2], parent->fields);
    }
  }
  CaValue annotations = ca_namespace_get(cls->namespace_, "__annotations__");
  if (annotations.tag == CA_DICT_T) {
    CaContainer *attrs = (CaContainer *)annotations.as.obj;
    for (size_t i = 0; i < attrs->length && !rt->error; i += 2) {
      CaValue name = attrs->items[i], type = attrs->items[i + 1];
      CaString *text = (CaString *)name.as.obj;
      CaValue existing = ca_namespace_get(roots[1], text->data);
      if (existing.tag == CA_UNBOUND_T)
        ca_append(rt, roots[2], name);
      CaValue default_value = ca_class_lookup(value, text->data),
              metadata = default_value;
      if (default_value.tag != CA_FOREIGN_T ||
          ((CaForeign *)default_value.as.obj)->kind != CA_F_FIELD)
        metadata = field(rt, default_value, CA_UNBOUND, type, name, ca_bool(1),
                         ca_bool(1));
      else {
        CaContainer *state =
            (CaContainer *)((CaForeign *)metadata.as.obj)->state.as.obj;
        state->items[2] = type;
        state->items[3] = name;
      }
      if (rt->error)
        break;
      ca_namespace_set(rt, roots[1], text->data, metadata);
    }
  }
  int saw_default = 0;
  if (!rt->error) {
    CaContainer *names = (CaContainer *)roots[2].as.obj;
    for (size_t i = 0; i < names->length; i++) {
      CaString *name = (CaString *)names->items[i].as.obj;
      CaValue metadata = ca_namespace_get(roots[1], name->data);
      CaForeign *field = (CaForeign *)metadata.as.obj;
      int provided = ca_capture(field->state, 0).tag != CA_UNBOUND_T ||
                     ca_capture(field->state, 1).tag != CA_UNBOUND_T;
      if (!provided && saw_default) {
        ca_error(rt, "TypeError",
                 "non-default argument follows default argument");
        break;
      }
      saw_default |= provided;
    }
  }
  if (!rt->error) {
    cls->fields = roots[2];
    cls->dataclass = 15;
    ca_namespace_set(rt, cls->namespace_, "__dataclass_fields__", roots[1]);
  }
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : value;
}
CaValue ca_dataclass_construct(CaRuntime *rt, CaValue type, int n, int nkw,
                               CaValue *args, const char **keys) {
  CaClass *cls = (CaClass *)type.as.obj;
  CaContainer *names = (CaContainer *)cls->fields.as.obj;
  if ((size_t)n > names->length) {
    ca_error(rt, "TypeError", "too many dataclass constructor arguments");
    return CA_UNBOUND;
  }
  for (int i = 0; i < nkw; i++) {
    int found = 0;
    for (size_t j = 0; j < names->length; j++) {
      CaString *name = (CaString *)names->items[j].as.obj;
      if (!strcmp(keys[i], name->data)) {
        if (j < (size_t)n) {
          ca_error(rt, "TypeError", "multiple values for dataclass field");
          return CA_UNBOUND;
        }
        found = 1;
        break;
      }
    }
    if (!found) {
      ca_error(rt, "TypeError", "unexpected dataclass constructor argument");
      return CA_UNBOUND;
    }
  }
  CaInstance *instance = ca_allocate(rt, sizeof *instance, CA_INSTANCE_T);
  if (!instance)
    return CA_UNBOUND;
  instance->type = type;
  instance->namespace_ = ca_container(rt, CA_DICT_T, 0, NULL);
  CaValue roots[] = {{.tag = CA_INSTANCE_T, .as.obj = (CaObj *)instance},
                     CA_UNBOUND};
  CaFrame frame;
  ca_push(rt, &frame, roots, 2);
  for (size_t i = 0; i < names->length && !rt->error; i++) {
    CaString *name = (CaString *)names->items[i].as.obj;
    CaValue value = i < (size_t)n ? args[i] : CA_UNBOUND;
    if (value.tag == CA_UNBOUND_T)
      for (int k = 0; k < nkw; k++)
        if (!strcmp(keys[k], name->data)) {
          value = args[n + k];
          break;
        }
    if (value.tag == CA_UNBOUND_T) {
      CaValue metadata = field_for(type, name->data);
      CaValue state = ((CaForeign *)metadata.as.obj)->state;
      value = ca_capture(state, 0);
      if (value.tag == CA_UNBOUND_T) {
        CaValue factory = ca_capture(state, 1);
        if (factory.tag != CA_UNBOUND_T)
          value = ca_call(rt, factory, 0, 0, NULL, NULL);
      }
    }
    if (rt->error)
      break;
    if (value.tag == CA_UNBOUND_T) {
      ca_error(rt, "TypeError", "missing required dataclass argument");
      break;
    }
    roots[1] = value;
    ca_namespace_set(rt, instance->namespace_, name->data, value);
  }
  if (!rt->error) {
    CaValue post = ca_try_special(rt, roots[0], "__post_init__");
    if (post.tag != CA_UNBOUND_T)
      ca_call(rt, post, 0, 0, NULL, NULL);
  }
  CaValue result = roots[0];
  ca_pop(rt, &frame);
  return rt->error ? CA_UNBOUND : result;
}
static CaValue asdict_value(CaRuntime *rt, CaValue value, CaValue factory) {
  if (!ca_enter(rt))
    return CA_UNBOUND;
  CaValue roots[] = {value, CA_UNBOUND, CA_UNBOUND, CA_UNBOUND, factory};
  CaFrame frame;
  ca_push(rt, &frame, roots, 5);
  if (value.tag == CA_INSTANCE_T &&
      ((CaClass *)((CaInstance *)value.as.obj)->type.as.obj)->dataclass) {
    CaInstance *instance = (CaInstance *)value.as.obj;
    CaClass *cls = (CaClass *)instance->type.as.obj;
    CaContainer *names = (CaContainer *)cls->fields.as.obj;
    roots[1] = ca_container(rt, CA_LIST_T, 0, NULL);
    for (size_t i = 0; i < names->length && !rt->error; i++) {
      CaString *name = (CaString *)names->items[i].as.obj;
      roots[2] = ca_attribute(rt, value, name->data, 0);
      if (rt->error)
        break;
      roots[3] = asdict_value(rt, roots[2], factory);
      if (rt->error)
        break;
      CaValue pair[] = {names->items[i], roots[3]};
      CaValue tuple = ca_container(rt, CA_TUPLE_T, 2, pair);
      if (!rt->error)
        ca_append(rt, roots[1], tuple);
    }
    if (!rt->error) {
      if (factory.tag == CA_NONE_T)
        roots[1] = ca_dict_constructor(rt, 1, 0, &roots[1], NULL);
      else
        roots[1] = ca_call(rt, factory, 1, 0, &roots[1], NULL);
    }
  } else if (value.tag == CA_LIST_T || value.tag == CA_TUPLE_T) {
    CaContainer *source = (CaContainer *)value.as.obj;
    roots[1] = ca_container(rt, CA_LIST_T, 0, NULL);
    for (size_t i = 0; i < source->length && !rt->error; i++) {
      roots[2] = asdict_value(rt, source->items[i], factory);
      if (!rt->error)
        ca_append(rt, roots[1], roots[2]);
    }
    if (value.tag == CA_TUPLE_T && !rt->error)
      roots[1] = ca_collection_builtin(rt, 5, 1, 0, &roots[1]);
  } else if (value.tag == CA_DICT_T) {
    CaContainer *source = (CaContainer *)value.as.obj;
    roots[1] = ca_container(rt, CA_DICT_T, 0, NULL);
    for (size_t i = 0; i < source->length && !rt->error; i += 2) {
      roots[2] = asdict_value(rt, source->items[i], factory);
      if (rt->error)
        break;
      roots[3] = asdict_value(rt, source->items[i + 1], factory);
      if (!rt->error)
        ca_setitem(rt, roots[1], roots[2], roots[3]);
    }
  } else if (value.tag == CA_INSTANCE_T ||
             (value.tag == CA_FOREIGN_T &&
              ((CaForeign *)value.as.obj)->kind != CA_F_PATH &&
              ((CaForeign *)value.as.obj)->kind != CA_F_UUID &&
              ((CaForeign *)value.as.obj)->kind != CA_F_DATETIME &&
              ((CaForeign *)value.as.obj)->kind != CA_F_UTC))
    ca_error(rt, "NotImplementedError",
             "asdict deep-copy for this native object is not implemented");
  else
    roots[1] = value;
  CaValue result = roots[1];
  ca_pop(rt, &frame);
  ca_leave(rt);
  return rt->error ? CA_UNBOUND : result;
}
CaValue ca_data_call(CaRuntime *rt, int kind, int n, int nkw, CaValue *args,
                     const char **keys) {
  if (kind == N_DATACLASS) {
    int slots = 0;
    if (n > 1)
      return ca_dep_error(rt, "TypeError",
                          "dataclass accepts at most one class");
    for (int i = 0; i < nkw; i++) {
      if (strcmp(keys[i], "slots"))
        return ca_dep_error(
            rt, "NotImplementedError",
            "native dataclass supports default options and slots only");
      slots = ca_truth(rt, args[n + i]);
    }
    if (!n) {
      if (!nkw)
        return ca_native_function(N_DATACLASS);
      CaValue values[] = {ca_native_function(N_DATACLASS), ca_bool(slots)};
      const char *names[] = {"slots"};
      return ca_data_call(rt, N_PARTIAL, 1, 1, values, names);
    }
    if (!slots)
      return dataclass_decorate(rt, args[0]);
    if (args[0].tag != CA_CLASS_T)
      return ca_dep_error(rt, "TypeError", "dataclass expects a class");
    CaClass *old = (CaClass *)args[0].as.obj;
    if (old->parent.tag == CA_CLASS_T)
      return ca_dep_error(rt, "NotImplementedError",
                          "slotted dataclass inheritance is not implemented");
    CaValue roots[] = {
        args[0], ca_class(rt, ((CaString *)old->name.as.obj)->data, 0, NULL),
        CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 3);
    if (!rt->error) {
      CaClass *cls = (CaClass *)roots[1].as.obj;
      ca_kwargs_merge(rt, cls->namespace_, old->namespace_, 0);
      roots[2] = dataclass_decorate(rt, roots[1]);
      if (!rt->error) {
        CaContainer *fields = (CaContainer *)cls->fields.as.obj;
        CaValue slots_value =
            ca_container(rt, CA_TUPLE_T, fields->length, fields->items);
        ca_namespace_set(rt, cls->namespace_, "__slots__", slots_value);
        cls->slotted = 1;
        for (size_t i = 0; i < fields->length && !rt->error; i++) {
          CaString *name = (CaString *)fields->items[i].as.obj;
          ca_namespace_set(rt, cls->namespace_, name->data,
                           ca_native_object(rt, 80, fields->items[i], NULL, 0));
        }
      }
    }
    CaValue result = roots[2];
    ca_pop(rt, &frame);
    return rt->error ? CA_UNBOUND : result;
  }
  if (kind == N_FIELD) {
    const char *names[] = {"default", "default_factory", "init",     "repr",
                           "hash",    "compare",         "metadata", "kw_only"};
    CaValue defaults[] = {CA_UNBOUND, CA_UNBOUND, ca_bool(1), ca_bool(1),
                          CA_NONE,    ca_bool(1), CA_NONE,    ca_bool(0)},
            out[8];
    if (n) {
      ca_error(rt, "TypeError", "field accepts keyword-only arguments");
      return CA_UNBOUND;
    }
    /* MISSING is a meaningful default, distinct from a missing required input.
     */
    for (int i = 0; i < 8; i++)
      out[i] = defaults[i];
    for (int i = 0; i < nkw; i++) {
      int slot = -1;
      for (int j = 0; j < 8; j++)
        if (!strcmp(keys[i], names[j]))
          slot = j;
      if (slot < 0) {
        ca_error(rt, "TypeError", "unexpected field keyword");
        return CA_UNBOUND;
      }
      out[slot] = args[i];
    }
    if (out[0].tag != CA_UNBOUND_T && out[1].tag != CA_UNBOUND_T) {
      ca_error(rt, "ValueError",
               "cannot specify both default and default_factory");
      return CA_UNBOUND;
    }
    if (!ca_truth(rt, out[2]) || out[4].tag != CA_NONE_T ||
        out[6].tag != CA_NONE_T || ca_truth(rt, out[7])) {
      ca_error(rt, "NotImplementedError",
               "field init=False, hash, metadata and keyword-only fields are "
               "not implemented");
      return CA_UNBOUND;
    }
    return field(rt, out[0], out[1], CA_NONE, CA_NONE,
                 ca_bool(ca_truth(rt, out[3])), ca_bool(ca_truth(rt, out[5])));
  }
  if (kind == N_ASDICT) {
    const char *names[] = {"obj", "dict_factory"};
    CaValue defaults[] = {CA_NONE, CA_NONE}, out[2];
    if (!ca_native_arguments(rt, n, nkw, args, keys, 2, 1, names, defaults,
                             out))
      return CA_UNBOUND;
    if (out[0].tag != CA_INSTANCE_T ||
        !((CaClass *)((CaInstance *)out[0].as.obj)->type.as.obj)->dataclass) {
      ca_error(rt, "TypeError",
               "asdict() should be called on dataclass instances");
      return CA_UNBOUND;
    }
    return asdict_value(rt, out[0], out[1]);
  }
  if (kind == N_PARTIAL) {
    if (n < 1) {
      ca_error(rt, "TypeError", "partial requires a callable");
      return CA_UNBOUND;
    }
    CaValue callable = ca_extended_call(rt, 35, 1, 0, args, NULL);
    if (rt->error)
      return CA_UNBOUND;
    if (!callable.as.i) {
      ca_error(rt, "TypeError", "the first argument must be callable");
      return CA_UNBOUND;
    }
    CaValue roots[] = {args[0],
                       ca_container(rt, CA_TUPLE_T, (size_t)n - 1, args + 1),
                       ca_container(rt, CA_DICT_T, 0, NULL)};
    CaFrame frame;
    ca_push(rt, &frame, roots, 3);
    for (int i = 0; i < nkw && !rt->error; i++) {
      CaValue key = ca_string(rt, keys[i], strlen(keys[i]));
      if (!rt->error)
        ca_setitem(rt, roots[2], key, args[n + i]);
    }
    CaValue state =
        rt->error ? CA_UNBOUND : ca_container(rt, CA_TUPLE_T, 3, roots);
    CaValue result = rt->error
                         ? CA_UNBOUND
                         : ca_native_object(rt, CA_F_PARTIAL, state, NULL, 0);
    ca_pop(rt, &frame);
    return result;
  }
  if (kind == N_PARTIAL_INVOKE) {
    CaForeign *partial = (CaForeign *)args[0].as.obj;
    CaValue roots[] = {ca_capture(partial->state, 0),
                       ca_capture(partial->state, 1),
                       ca_capture(partial->state, 2), CA_UNBOUND, CA_UNBOUND};
    CaFrame frame;
    ca_push(rt, &frame, roots, 5);
    roots[3] = ca_container(rt, CA_TUPLE_T, (size_t)n - 1, args + 1);
    if (!rt->error)
      roots[3] = ca_sequence_binary(rt, roots[1], roots[3], '+');
    if (!rt->error) {
      CaContainer *old = (CaContainer *)roots[2].as.obj;
      roots[4] = ca_container(rt, CA_DICT_T, old->length, old->items);
    }
    for (int i = 0; i < nkw && !rt->error; i++) {
      CaValue key = ca_string(rt, keys[i], strlen(keys[i]));
      if (!rt->error)
        ca_setitem(rt, roots[4], key, args[n + i]);
    }
    CaValue result =
        rt->error ? CA_UNBOUND : ca_call_ex(rt, roots[0], roots[3], roots[4]);
    ca_pop(rt, &frame);
    return result;
  }
#ifdef CA_WITH_OPENSSL
  if (kind == N_SHA256) {
    CaValue out[2], defaults[] = {ca_bytes(rt, "", 0), ca_bool(1)};
    const char *names[] = {"string", "usedforsecurity"};
    if (!ca_native_arguments(rt, n, nkw, args, keys, 2, 0, names, defaults,
                             out))
      return CA_UNBOUND;
    if (out[0].tag != CA_BYTES_T) {
      ca_error(rt, "TypeError", "hash data must be bytes");
      return CA_UNBOUND;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context) {
      ca_error(rt, "MemoryError", "hash context allocation failed");
      return CA_UNBOUND;
    }
    CaString *data = (CaString *)out[0].as.obj;
    if (!EVP_DigestInit_ex(context, EVP_sha256(), NULL) ||
        !EVP_DigestUpdate(context, data->data, data->length)) {
      EVP_MD_CTX_free(context);
      ca_error(rt, "RuntimeError", "OpenSSL hash initialization failed");
      return CA_UNBOUND;
    }
    CaValue result = ca_native_object(rt, CA_F_HASH, CA_NONE, context, 0);
    if (rt->error)
      EVP_MD_CTX_free(context);
    return result;
  }
  if (kind >= N_HASH_UPDATE && kind <= N_HASH_COPY) {
    CaValue out[2], defaults[] = {CA_NONE, CA_NONE};
    const char *names[] = {"self", "data"};
    if (!ca_native_arguments(
            rt, n, nkw, args, keys, kind == N_HASH_UPDATE ? 2 : 1,
            kind == N_HASH_UPDATE ? 2 : 1, names, defaults, out))
      return CA_UNBOUND;
    if (out[0].tag != CA_FOREIGN_T ||
        ((CaForeign *)out[0].as.obj)->kind != CA_F_HASH) {
      ca_error(rt, "TypeError", "hash method requires hash object");
      return CA_UNBOUND;
    }
    EVP_MD_CTX *original = ((CaForeign *)out[0].as.obj)->handle;
    if (kind == N_HASH_UPDATE) {
      if (out[1].tag != CA_BYTES_T) {
        ca_error(rt, "TypeError", "hash data must be bytes");
        return CA_UNBOUND;
      }
      CaString *data = (CaString *)out[1].as.obj;
      if (!EVP_DigestUpdate(original, data->data, data->length)) {
        ca_error(rt, "RuntimeError", "OpenSSL digest update failed");
        return CA_UNBOUND;
      }
      return CA_NONE;
    }
    EVP_MD_CTX *copy = EVP_MD_CTX_new();
    if (!copy) {
      ca_error(rt, "MemoryError", "hash context allocation failed");
      return CA_UNBOUND;
    }
    if (!EVP_MD_CTX_copy_ex(copy, original)) {
      EVP_MD_CTX_free(copy);
      ca_error(rt, "RuntimeError", "OpenSSL digest copy failed");
      return CA_UNBOUND;
    }
    if (kind == N_HASH_COPY) {
      CaValue result = ca_native_object(rt, CA_F_HASH, CA_NONE, copy, 0);
      if (rt->error)
        EVP_MD_CTX_free(copy);
      return result;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    int ok = EVP_DigestFinal_ex(copy, digest, &length);
    EVP_MD_CTX_free(copy);
    if (!ok) {
      ca_error(rt, "RuntimeError", "OpenSSL digest finalization failed");
      return CA_UNBOUND;
    }
    if (kind == N_HASH_DIGEST)
      return ca_bytes(rt, (const char *)digest, length);
    char hex[EVP_MAX_MD_SIZE * 2];
    const char *digits = "0123456789abcdef";
    for (unsigned i = 0; i < length; i++) {
      hex[i * 2] = digits[digest[i] >> 4];
      hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    return ca_string(rt, hex, (size_t)length * 2);
  }
#endif
  ca_error(rt, "NotImplementedError",
           "native data API overload is not implemented");
  return CA_UNBOUND;
}
