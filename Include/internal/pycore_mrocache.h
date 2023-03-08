#ifndef Py_INTERNAL_TYPECACHE_H
#define Py_INTERNAL_TYPECACHE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// TODO(sgross): MRO cache or type cache?

typedef struct _Py_mro_cache_entry {
    PyObject *name;     /* name (interned unicode; immortal) */
    PyObject *value;    /* resolved function (owned ref) */
} _Py_mro_cache_entry;

typedef struct _Py_mro_cache_buckets {
    union {
        Py_ssize_t refcount;
        Py_ssize_t capacity;
    } u;
    uint32_t available; /* number of unused buckets */
    uint32_t used;      /* number of used buckets */
    _Py_mro_cache_entry array[];
} _Py_mro_cache_buckets;

/* Per-interpreter state */
struct _mro_cache_state {
    _Py_mro_cache_buckets *empty_buckets;
    Py_ssize_t empty_buckets_capacity;
};

extern PyStatus _Py_mro_cache_init(PyInterpreterState *interp);
extern void _Py_mro_cache_fini(PyInterpreterState *interp);
extern void _Py_mro_cache_init_type(PyTypeObject *type);
extern void _Py_mro_cache_fini_type(PyTypeObject *type);
extern int _Py_mro_cache_visit(_Py_mro_cache *cache, visitproc visit, void *arg);

extern void _Py_mro_cache_erase(_Py_mro_cache *cache);
extern void _Py_mro_cache_insert(_Py_mro_cache *cache, PyObject *name, PyObject *value);

extern PyObject *_Py_mro_cache_as_dict(_Py_mro_cache *cache);
extern PyObject _Py_NotFoundStruct;

static inline PyObject *
_Py_mro_cache_lookup(_Py_mro_cache *cache, PyObject *name)
{
    Py_hash_t hash = ((PyASCIIObject *)name)->hash;
    uint32_t mask = _Py_atomic_load_uint32(&cache->mask);
    _Py_mro_cache_entry *first = _Py_atomic_load_ptr_relaxed(&cache->buckets);

    Py_ssize_t offset = hash & mask;
    _Py_mro_cache_entry *bucket = (_Py_mro_cache_entry *)((char *)first + offset);

    PyObject *entry_name = _Py_atomic_load_ptr_relaxed(&bucket->name);
    if (_PY_LIKELY(entry_name == name)) {
        return _Py_atomic_load_ptr_relaxed(&bucket->value);
    }

    /* First loop */
    while (1) {
        if (entry_name == NULL) {
            return NULL;
        }
        if (bucket == first) {
            break;
        }
        bucket--;
        entry_name = _Py_atomic_load_ptr_relaxed(&bucket->name);
        if (entry_name == name) {
            return _Py_atomic_load_ptr_relaxed(&bucket->value);
        }
    }

    /* Second loop. Start at the last bucket. */
    bucket = (_Py_mro_cache_entry *)((char *)first + mask);
    while (1) {
        entry_name = _Py_atomic_load_ptr_relaxed(&bucket->name);
        if (entry_name == name) {
            return _Py_atomic_load_ptr_relaxed(&bucket->value);
        }
        if (entry_name == NULL || bucket == first) {
            return NULL;
        }
        bucket--;
    }
}


#ifdef __cplusplus
}
#endif
#endif   /* !Py_INTERNAL_TYPECACHE_H */
