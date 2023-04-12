#include "Python.h"

#include "pycore_initconfig.h"
#include "pycore_interp.h"
#include "pycore_mrocache.h"
#include "pycore_pymem.h"
#include "pycore_pystate.h"

#include <stdbool.h>
#include <stddef.h>

#define _Py_MRO_CACHE_MIN_SIZE 8
#define _Py_MRO_CACHE_MAX_SIZE 65536

static PyObject *
not_found_repr(PyObject *self)
{
    return PyUnicode_FromString("<not found>");
}

static PyTypeObject _PyNotFound_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "<not found> type",
    .tp_repr = not_found_repr,
};

PyObject _Py_NotFoundStruct = _PyObject_STRUCT_INIT(&_PyNotFound_Type);

/* NOTE: mask is used to index array in bytes */
static uint32_t
mask_from_capacity(size_t capacity)
{
    assert((capacity & (capacity - 1)) == 0);
    assert(capacity >= _Py_MRO_CACHE_MIN_SIZE);

    return (uint32_t)((capacity - 1) * sizeof(_Py_mro_cache_entry));
}

static size_t
capacity_from_mask(Py_ssize_t mask)
{
    return (mask / sizeof(_Py_mro_cache_entry)) + 1;
}

static void
decref_empty_bucket(_Py_mro_cache_buckets *buckets)
{
    assert(buckets->u.refcount > 0);
    buckets->u.refcount--;
    if (buckets->u.refcount == 0) {
        _PyMem_FreeQsbr(buckets);
    }
}

static void
buckets_free(void *ptr)
{
    _Py_mro_cache_buckets *buckets = (_Py_mro_cache_buckets *)ptr;
    Py_ssize_t capacity = buckets->u.capacity;
    for (Py_ssize_t i = 0; i < capacity; i++) {
        Py_XDECREF(buckets->array[i].value);
    }
    PyMem_Free(buckets);
}

static void
clear_buckets(_Py_mro_cache_buckets *buckets)
{
    if (buckets->used == 0 && buckets->available == 0) {
        decref_empty_bucket(buckets);
    }
    else {
        _PyQsbr_Free(buckets, &buckets_free);
    }
}

static _Py_mro_cache_buckets *
allocate_empty_buckets(Py_ssize_t capacity)
{
    Py_ssize_t size = sizeof(_Py_mro_cache_buckets) + capacity * sizeof(_Py_mro_cache_entry);
    _Py_mro_cache_buckets *buckets = PyMem_Calloc(1, size);
    buckets->u.refcount = 1;
    return buckets;
}

static _Py_mro_cache_buckets *
get_buckets(_Py_mro_cache *cache)
{
    char *mem = (char *)cache->buckets;
    mem -= offsetof(_Py_mro_cache_buckets, array);
    return (_Py_mro_cache_buckets *)mem;
}

static _Py_mro_cache_buckets *
allocate_buckets(Py_ssize_t capacity)
{
    if (capacity > _Py_MRO_CACHE_MAX_SIZE) {
        return NULL;
    }

    /* Ensure that there is an empty buckets array of at least the same capacity. */
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (capacity > (Py_ssize_t)interp->mro_cache.empty_buckets_capacity) {
        _Py_mro_cache_buckets *old = interp->mro_cache.empty_buckets;
        _Py_mro_cache_buckets *new = allocate_empty_buckets(capacity);
        if (new == NULL) {
            return NULL;
        }
        interp->mro_cache.empty_buckets = new;
        interp->mro_cache.empty_buckets_capacity = capacity;
        decref_empty_bucket(old);
    }

    Py_ssize_t size = sizeof(_Py_mro_cache_buckets) + capacity * sizeof(_Py_mro_cache_entry);
    _Py_mro_cache_buckets *buckets = PyMem_Calloc(1, size);
    if (buckets == NULL) {
        return NULL;
    }
    buckets->u.capacity = capacity;
    buckets->available = (capacity + 1) * 7 / 8;
    buckets->used = 0;
    return buckets;
}

void
_Py_mro_cache_erase(_Py_mro_cache *cache)
{
    _Py_mro_cache_buckets *old = get_buckets(cache);
    if (old->available == 0 && old->used == 0) {
        return;
    }

    PyInterpreterState *interp = _PyInterpreterState_GET();
    struct _mro_cache_state *mro_cache = &interp->mro_cache;
    assert(capacity_from_mask(cache->mask) <= (size_t)mro_cache->empty_buckets_capacity);

    _Py_mro_cache_buckets *empty_buckets = mro_cache->empty_buckets;
    empty_buckets->u.refcount++;
    _Py_atomic_store_ptr_release(&cache->buckets, empty_buckets->array);

    _PyQsbr_Free(old, &buckets_free);
}

static int
resize(_Py_mro_cache *cache, _Py_mro_cache_buckets *buckets)
{
    size_t old_capacity = capacity_from_mask(cache->mask);
    size_t new_capacity;
    if (buckets->used == 0) {
        /* empty bucket */
        new_capacity = old_capacity;
    }
    else {
        new_capacity = old_capacity * 2;
    }
    uint32_t new_mask = mask_from_capacity(new_capacity);

    _Py_mro_cache_buckets *new_buckets = allocate_buckets(new_capacity);
    if (new_buckets == NULL) {
        return -1;
    }

    // First store the new buckets.
    _Py_atomic_store_ptr_release(&cache->buckets, new_buckets->array);

    // Then update the mask (with at least release semantics) so that
    // the buckets is visible first.
    _Py_atomic_store_uint32(&cache->mask, new_mask);

    clear_buckets(buckets);
    return 0;
}

void
_Py_mro_cache_insert(_Py_mro_cache *cache, PyObject *name, PyObject *value)
{
    assert(PyUnicode_CheckExact(name) && PyUnicode_CHECK_INTERNED(name));

    if (value == NULL) {
        value = &_Py_NotFoundStruct;
    }

    _Py_mro_cache_buckets *buckets = get_buckets(cache);
    if (buckets->available == 0) {
        if (resize(cache, buckets) < 0) {
            // allocation failure: don't cache the value
            return;
        }
        buckets = get_buckets(cache);
        assert(buckets->available > 0);
    }

    assert(buckets->available < UINT32_MAX/10);

    Py_hash_t hash = ((PyASCIIObject *)name)->hash;
    Py_ssize_t capacity = capacity_from_mask(cache->mask);
    Py_ssize_t ix = (hash & cache->mask) / sizeof(_Py_mro_cache_entry);
    for (;;) {
        if (buckets->array[ix].name == NULL) {
            buckets->array[ix].name = name;
            buckets->array[ix].value = Py_NewRef(value);
            assert(buckets->available > 0);
            buckets->available--;
            buckets->used++;
            return;
        }
        else if (buckets->array[ix].name == name) {
            /* someone else added the entry before us. */
            return;
        }
        ix = (ix == 0) ? capacity - 1 : ix - 1;
    }    
}

PyObject *
_Py_mro_cache_as_dict(_Py_mro_cache *cache)
{
    PyObject *dict = PyDict_New();
    if (dict == NULL) {
        return NULL;
    }

    _Py_mro_cache_entry *entry = cache->buckets;
    Py_ssize_t capacity = capacity_from_mask(cache->mask);
    for (Py_ssize_t i = 0; i < capacity; i++, entry++) {
        if (entry->name) {
            int err = PyDict_SetItem(dict, entry->name, entry->value);
            if (err < 0) {
                Py_CLEAR(dict);
                return NULL;
            }
        }
    }

    return dict;
}

void
_Py_mro_cache_init_type(PyTypeObject *type)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (type->tp_mro_cache.buckets == NULL) {
        struct _Py_mro_cache_buckets *empty_buckets = interp->mro_cache.empty_buckets;
        empty_buckets->u.refcount++;
        type->tp_mro_cache.buckets = empty_buckets->array;
        type->tp_mro_cache.mask = mask_from_capacity(_Py_MRO_CACHE_MIN_SIZE);
    }
}

void
_Py_mro_cache_fini_type(PyTypeObject *type)
{
    if (type->tp_mro_cache.buckets != NULL) {
        _Py_mro_cache_buckets *buckets = get_buckets(&type->tp_mro_cache);
        type->tp_mro_cache.buckets = NULL;
        type->tp_mro_cache.mask = 0;
        clear_buckets(buckets);
    }
}

int
_Py_mro_cache_visit(_Py_mro_cache *cache, visitproc visit, void *arg)
{
    _Py_mro_cache_entry *entry = cache->buckets;
    if (entry == NULL) {
        return 0;
    }
    Py_ssize_t capacity = capacity_from_mask(cache->mask);
    for (Py_ssize_t i = 0; i < capacity; i++, entry++) {
        if (entry->value) {
            int err = visit(entry->value, arg);
            if (err != 0) {
                return err;
            }
        }
    }
    return 0;
}

PyStatus
_Py_mro_cache_init(PyInterpreterState *interp)
{
    _Py_mro_cache_buckets *b = allocate_empty_buckets(_Py_MRO_CACHE_MIN_SIZE);
    if (b == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    interp->mro_cache.empty_buckets = b;
    interp->mro_cache.empty_buckets_capacity = _Py_MRO_CACHE_MIN_SIZE;
    return _PyStatus_OK();
}

void
_Py_mro_cache_fini(PyInterpreterState *interp)
{
    _Py_mro_cache_buckets *b = interp->mro_cache.empty_buckets;
    if (b != NULL) {
        interp->mro_cache.empty_buckets = NULL;
        interp->mro_cache.empty_buckets_capacity = 0;
        decref_empty_bucket(b);
    }
}