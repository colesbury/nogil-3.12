/*

  Reference Cycle Garbage Collection
  ==================================

  Neil Schemenauer <nas@arctrix.com>

  Based on a post on the python-dev list.  Ideas from Guido van Rossum,
  Eric Tiedemann, and various others.

  http://www.arctrix.com/nas/python/gc/

  The following mailing list threads provide a historical perspective on
  the design of this module.  Note that a fair amount of refinement has
  occurred since those discussions.

  http://mail.python.org/pipermail/python-dev/2000-March/002385.html
  http://mail.python.org/pipermail/python-dev/2000-March/002434.html
  http://mail.python.org/pipermail/python-dev/2000-March/002497.html

  For a highlevel view of the collection process, read the collect
  function.

*/

#include "Python.h"
#include "pycore_context.h"
#include "pycore_dict.h"
#include "pycore_initconfig.h"
#include "pycore_interp.h"      // PyInterpreterState.gc
#include "pycore_object.h"
#include "pycore_pyerrors.h"
#include "pycore_pymem.h"
#include "pycore_pystate.h"
#include "pycore_refcnt.h"
#include "pycore_qsbr.h"
#include "pycore_gc.h"
#include "frameobject.h"        /* for PyFrame_ClearFreeList */
#include "pydtrace.h"

#include "mimalloc.h"
#include "mimalloc-internal.h"

typedef struct _gc_runtime_state GCState;

/*[clinic input]
module gc
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=b5c9690ecc842d79]*/


#ifdef Py_DEBUG
#  define GC_DEBUG
#endif

#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV

/* Get an object's GC head */
#define AS_GC(o) ((PyGC_Head *)(((char *)(o))+PyGC_Head_OFFSET))

/* Get the object given the GC head */
#define FROM_GC(g) ((PyObject *)(((char *)(g))-PyGC_Head_OFFSET))

#define _PyGC_TRACKED(g) (((g)->_gc_prev & _PyGC_PREV_MASK_TRACKED) != 0)

typedef enum {
    /* GC was triggered by heap allocation */
    GC_REASON_HEAP,

    /* GC was called due to shutdown */
    GC_REASON_SHUTDOWN,

    /* GC was called via gc.collect() or PyGC_Collect */
    GC_REASON_MANUAL
} _PyGC_Reason;

static inline void
gc_set_unreachable(PyGC_Head *g)
{
    g->_gc_prev |= _PyGC_PREV_MASK_UNREACHABLE;
}

static inline int
gc_is_unreachable(PyGC_Head *g)
{
    return (g->_gc_prev & _PyGC_PREV_MASK_UNREACHABLE) != 0;
}

static inline int
gc_is_unreachable2(PyObject *op)
{
    return (op->ob_gc_bits & _PyGC_UNREACHABLE) != 0;
}

static inline Py_ssize_t
gc_get_refs(PyGC_Head *g)
{
    return ((Py_ssize_t)g->_gc_prev) >> _PyGC_PREV_SHIFT;
}

static inline void
gc_set_refs(PyGC_Head *g, Py_ssize_t refs)
{
    g->_gc_prev = (g->_gc_prev & ~_PyGC_PREV_MASK)
        | ((uintptr_t)(refs) << _PyGC_PREV_SHIFT);
}

static inline void
gc_add_refs(PyGC_Head *g, Py_ssize_t refs)
{
    g->_gc_prev += (refs << _PyGC_PREV_SHIFT);
}

static inline void
gc_decref(PyGC_Head *g)
{
    g->_gc_prev -= 1 << _PyGC_PREV_SHIFT;
}

/* set for debugging information */
#define DEBUG_STATS             (1<<0) /* print collection statistics */
#define DEBUG_COLLECTABLE       (1<<1) /* print collectable objects */
#define DEBUG_UNCOLLECTABLE     (1<<2) /* print uncollectable objects */
#define DEBUG_SAVEALL           (1<<5) /* save all garbage in gc.garbage */
#define DEBUG_LEAK              DEBUG_COLLECTABLE | \
                DEBUG_UNCOLLECTABLE | \
                DEBUG_SAVEALL


static inline void gc_list_init(PyGC_Head *list);


static GCState *
get_gc_state(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->gc;
}


void
_PyGC_InitState(GCState *gcstate)
{
    gcstate->enabled = 1; /* automatic collection enabled? */
    gcstate->gc_live = 0;
    gcstate->gc_threshold = 7000;
    gcstate->gc_scale = 25;

    const char* scale_str = _Py_GetEnv(1, "PYTHONGC");
    if (scale_str) {
        (void)_Py_str_to_int(scale_str, &gcstate->gc_scale);
    }
}


PyStatus
_PyGC_Init(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;

    gcstate->garbage = PyList_New(0);
    if (gcstate->garbage == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    gcstate->callbacks = PyList_New(0);
    if (gcstate->callbacks == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    return _PyStatus_OK();
}


/*
_gc_prev values
---------------

Between collections, _gc_prev is used for doubly linked list.

Lowest two bits of _gc_prev are used for flags.
PREV_MASK_COLLECTING is used only while collecting and cleared before GC ends
or _PyObject_GC_UNTRACK() is called.

During a collection, _gc_prev is temporary used for gc_refs, and the gc list
is singly linked until _gc_prev is restored.

gc_refs
    At the start of a collection, update_refs() copies the true refcount
    to gc_refs, for each object in the generation being collected.
    subtract_refs() then adjusts gc_refs so that it equals the number of
    times an object is referenced directly from outside the generation
    being collected.

PREV_MASK_COLLECTING
    Objects in generation being collected are marked PREV_MASK_COLLECTING in
    update_refs().


_gc_next values
---------------

_gc_next takes these values:

0
    The object is not tracked

!= 0
    Pointer to the next object in the GC list.
    Additionally, lowest bit is used temporary for
    NEXT_MASK_UNREACHABLE flag described below.

NEXT_MASK_UNREACHABLE
    move_unreachable() then moves objects not reachable (whether directly or
    indirectly) from outside the generation into an "unreachable" set and
    set this flag.

    Objects that are found to be reachable have gc_refs set to 1.
    When this flag is set for the reachable object, the object must be in
    "unreachable" set.
    The flag is unset and the object is moved back to "reachable" set.

    move_legacy_finalizers() will remove this flag from "unreachable" set.
*/

/*** list functions ***/

static inline void
gc_list_init(PyGC_Head *list)
{
    // List header must not have flags.
    // We can assign pointer by simple cast.
    list->_gc_prev = (uintptr_t)list;
    list->_gc_next = (uintptr_t)list;
}

static inline int
gc_list_is_empty(PyGC_Head *list)
{
    return (list->_gc_next == (uintptr_t)list);
}

static Py_ssize_t
_Py_GC_REFCNT(PyObject *op)
{
    Py_ssize_t local, shared;
    int immortal, deferred;

    _PyRef_UnpackLocal(op->ob_ref_local, &local, &immortal, &deferred);
    _PyRef_UnpackShared(op->ob_ref_shared, &shared, NULL, NULL);
    assert(!immortal);

    return local + shared - deferred;
}

typedef int (gc_visit_fn)(PyGC_Head* gc, void *arg);

/* True if memory is allocated by the debug allocator.
 * See obmalloc.c
 */
static int using_debug_allocator;

static int
visit_page(const mi_page_t* page, gc_visit_fn* visitor, void *arg)
{
    mi_segment_t* segment = _mi_page_segment(page);
    size_t block_size = page->xblock_size;
    uint8_t *data = _mi_page_start(segment, page, NULL);
    // printf("visiting page %p of size %zu capacity %d debug=%d\n", page, block_size, (int)page->capacity, (int)using_debug_allocator);
    for (int i = 0, end = page->capacity; i != end; i++) {
        uint8_t *p = data + i * block_size;
        if (using_debug_allocator) {
            /* The debug allocator sticks two words before each allocation.
             * When the allocation is active, the low bit of the first word
             * is set.
             */
            /* TODO(sgross): update and handle debug allocator in obmalloc.c */
            size_t *size_prefix = (size_t*)p;
            if (!(*size_prefix & 1)) {
                continue;
            }
            p += 2 * sizeof(size_t);
        }
        PyGC_Head *gc = (PyGC_Head *)p;
        if (_PyGC_TRACKED(gc)) {
            int err = (*visitor)(gc, arg);
            if (err) {
                return err;
            }
        }
    }
    return 0;
}

static int
visit_segments(mi_segment_t* segment, gc_visit_fn* visitor, void *arg)
{
    while (segment) {
        const mi_slice_t* end;
        mi_slice_t* slice = mi_slices_start_iterate(segment, &end);
        while (slice < end) {
            if (slice->xblock_size > 0) {
                mi_page_t* const page = mi_slice_to_page(slice);
                if (page->tag == mi_heap_tag_gc) {
                    int err = visit_page(page, visitor, arg);
                    if (err) {
                        return err;
                    }
                }
            }
            slice = slice + slice->slice_count;
        }
        segment = segment->abandoned_next;
    }
    return 0;
}

static int
visit_heap(mi_heap_t *heap, gc_visit_fn* visitor, void *arg)
{
    if (!heap || heap->visited || heap->page_count == 0) {
        return 0;
    }

    for (size_t i = 0; i <= MI_BIN_FULL; i++) {
        const mi_page_queue_t *pq = &heap->pages[i];
        mi_page_t *page = pq->first;
        while (page != NULL) {
            assert(page->tag == mi_heap_tag_gc);
            int err = visit_page(page, visitor, arg);
            if (err) {
                return err;
            }
            page = page->next;
        }
    }

    heap->visited = true;
    return 0;
}

static int
visit_heaps(gc_visit_fn* visitor, void *arg)
{
    int err = 0;
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *t;

    HEAD_LOCK(runtime);

    using_debug_allocator = _PyMem_DebugEnabled();

    for_each_thread(t) {
        mi_heap_t *heap = t->heaps[mi_heap_tag_gc];
        int err = visit_heap(heap, visitor, arg);
        if (err) {
            goto end;
        }
    }

    err = visit_segments(_mi_segment_abandoned(), visitor, arg);
    if (err) {
        goto end;
    }

    err = visit_segments(_mi_segment_abandoned_visited(), visitor, arg);
    if (err) {
        goto end;
    }

end:
    for_each_thread(t) {
        mi_heap_t *heap = t->heaps[mi_heap_tag_gc];
        if (heap) {
            heap->visited = false;
        }
    }

    HEAD_UNLOCK(runtime);
    return err;
}

struct debug_visitor_args {
    mi_block_visit_fun *visitor;
    void *arg;
};

static bool
debug_visitor(const mi_heap_t* heap, const mi_heap_area_t* area, void* block, size_t block_size, void* arg)
{
    struct debug_visitor_args *args = (struct debug_visitor_args *)arg;
    if (block) {
        block = (char *)block + 2 * sizeof(size_t);
    }
    block_size -= 2 * sizeof(size_t);
    return args->visitor(heap, area, block, block_size, args->arg);
}

static void
visit_heaps2(int heap_tag, mi_block_visit_fun *visitor, void *arg)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *t;
    struct debug_visitor_args debug_args = { visitor, arg };

    HEAD_LOCK(runtime);
    using_debug_allocator = _PyMem_DebugEnabled();

    // FIXME(sgross): dict keys don't go through debug allocator
    if (using_debug_allocator && heap_tag != mi_heap_tag_dict_keys) {
        visitor = debug_visitor;
        arg = &debug_args;
    }

    for_each_thread(t) {
        mi_heap_t *heap = t->heaps[heap_tag];
        if (heap && !heap->visited) {
            mi_heap_visit_blocks(heap, true, visitor, arg);
            heap->visited = true;
        }
    }

    _mi_abandoned_visit_blocks(heap_tag, true, visitor, arg);

    for_each_thread(t) {
        mi_heap_t *heap = t->heaps[heap_tag];
        if (heap) {
            heap->visited = false;
        }
    }

    HEAD_UNLOCK(runtime);
}

struct find_object_args {
    PyObject *op;
    int found;
};

static int
find_object_visitor(PyGC_Head* gc, void *arg)
{
    struct find_object_args *args = (struct find_object_args *)arg;
    if (FROM_GC(gc) == args->op) {
        args->found = 1;
    }
    return 0;
}

int
_PyGC_find_object(PyObject *op)
{
    struct find_object_args args;
    args.op = op;
    args.found = 0;
    visit_heaps(find_object_visitor, &args);
    return args.found;
}

// Constants for validate_list's flags argument.
enum flagstates {unreachable_clear,
                 unreachable_set};

#ifdef GC_DEBUG
// validate_list checks list consistency.  And it works as document
// describing when flags are expected to be set / unset.
// `head` must be a doubly-linked gc list, although it's fine (expected!) if
// the prev and next pointers are "polluted" with flags.
// What's checked:
// - The `head` pointers are not polluted.
// - The objects' PREV_MASK_COLLECTING and NEXT_MASK_UNREACHABLE flags are all
//   `set or clear, as specified by the 'flags' argument.
// - The prev and next pointers are mutually consistent.
static void
validate_list(PyGC_Head *head, enum flagstates flags)
{
    assert(!gc_is_unreachable(head));
    uintptr_t prev_mask = 0, prev_value = 0;
    switch (flags) {
        case unreachable_clear:
            prev_mask = _PyGC_PREV_MASK_UNREACHABLE;
            prev_value = 0;
            break;
        case unreachable_set:
            prev_mask = _PyGC_PREV_MASK_UNREACHABLE;
            prev_value = _PyGC_PREV_MASK_UNREACHABLE;
            break;
        default:
            assert(! "bad internal flags argument");
    }
    PyGC_Head *prev = head;
    PyGC_Head *gc = GC_NEXT(head);
    int n = 0;
    while (gc != head) {
        PyGC_Head *trueprev = GC_PREV(gc);
        PyGC_Head *truenext = (PyGC_Head *)(gc->_gc_next);
        assert(truenext != NULL);
        assert(trueprev == prev);
        assert((gc->_gc_prev & prev_mask) == prev_value);
        assert((gc->_gc_next & 3) == 0);
        prev = gc;
        gc = truenext;
        n++;
    }
    assert(prev == GC_PREV(head));
}

static int
validate_refcount_visitor(PyGC_Head* gc, void *arg)
{
    assert(_Py_GC_REFCNT(FROM_GC(gc)) > 0);
    return 0;
}

static void
validate_refcount(void)
{
    visit_heaps(validate_refcount_visitor, NULL);
}

struct validate_tracked_args {
    uintptr_t mask;
    uintptr_t expected;
};

static int
validate_tracked_visitor(PyGC_Head* gc, void *void_arg)
{
    struct validate_tracked_args *arg = (struct validate_tracked_args*)void_arg;
    assert((gc->_gc_prev & arg->mask) == arg->expected);
    assert(gc->_gc_next == 0);
    assert(_PyGCHead_PREV(gc) == NULL);
    assert(_Py_GC_REFCNT(FROM_GC(gc)) >= 0);
    return 0;
}

static void
validate_tracked_heap(uintptr_t mask, uintptr_t expected)
{
    struct validate_tracked_args args;
    args.mask = mask;
    args.expected = expected;
    visit_heaps(validate_tracked_visitor, &args);
}
#else
#define validate_list(x, y) do{}while(0)
#define validate_refcount() do{}while(0)
#define validate_tracked_heap(x,y) do{}while(0)
#endif

static int
reset_heap_visitor(PyGC_Head *gc, void *void_arg)
{
    gc->_gc_prev = 0;
    return 0;
}

void
_PyGC_ResetHeap(void)
{
    // NOTE: _PyGC_Initialize may be called multiple times. For example,
    // _test_embed triggers multiple GC initializations, including some
    // after _Py_Initialize failures. Since _Py_Initialize clears _PyRuntime
    // we have no choice but to leak all PyObjects.
    // TODO(sgross): should we drop mi_heap here instead?
    visit_heaps(reset_heap_visitor, NULL);
}

/* Subtracts incoming references. */
static int
visit_decref(PyObject *op, void *arg)
{
    if (_PyObject_IS_GC(op)) {
        PyGC_Head *gc = AS_GC(op);
        // We're only interested in gc_refs for tracked objects.
        if (_PyGC_TRACKED(gc)) {
            gc_decref(gc);
        }
    }
    return 0;
}

static void
find_dead_shared_keys(_PyObjectQueue **queue, int *num_unmarked)
{
    PyInterpreterState *interp = _PyRuntime.interpreters.head;
    while (interp) {
        struct _Py_dict_state *dict_state = &interp->dict_state;
        PyDictSharedKeysObject **prev_nextptr = &dict_state->tracked_shared_keys;
        PyDictSharedKeysObject *keys = dict_state->tracked_shared_keys;
        while (keys) {
            assert(keys->tracked);
            PyDictSharedKeysObject *next = keys->next;
            if (keys->marked) {
                keys->marked = 0;
                prev_nextptr = &keys->next;
                *num_unmarked += 1;
            }
            else {
                *prev_nextptr = next;
                // FIXME: bad cast
                _PyObjectQueue_Push(queue, (PyObject *)keys);
            }
            keys = next;
        }

        interp = interp->next;
    }
}

static void
merge_refcount(PyObject *op, Py_ssize_t extra)
{
    Py_ssize_t local_refcount, shared_refcount;
    int immortal, deferred;

    assert(_PyRuntime.stop_the_world);

    _PyRef_UnpackLocal(op->ob_ref_local, &local_refcount, &immortal, &deferred);
    _PyRef_UnpackShared(op->ob_ref_shared, &shared_refcount, NULL, NULL);
    assert(!immortal && "immortal objects should not be in garbage");

    Py_ssize_t refcount = local_refcount + shared_refcount;
    refcount += extra;
    refcount -= deferred;

#ifdef Py_REF_DEBUG
    _Py_IncRefTotalN(extra);
#endif

    op->ob_tid = 0;
    op->ob_ref_local = 0;
    op->ob_ref_shared = _Py_REF_PACK_SHARED(refcount, _Py_REF_MERGED);
}

struct update_refs_args {
    GCState *gcstate;
    int split_keys_marked;
    _PyGC_Reason gc_reason;
};

// Compute the number of external references to objects in the heap
// by subtracting internal references from the refcount.
static bool
update_refs(const mi_heap_t* heap, const mi_heap_area_t* area, void* block, size_t block_size, void* args)
{
    PyGC_Head *gc = (PyGC_Head *)block;
    if (!gc) return true;

    struct update_refs_args *arg = (struct update_refs_args *)args;

    PyObject *op = FROM_GC(gc);
    if (PyDict_CheckExact(op)) {
        PyDictObject *mp = (PyDictObject *)op;
        if (mp->ma_keys && mp->ma_keys->dk_kind == DICT_KEYS_SPLIT) {
            PyDictSharedKeysObject *shared = DK_AS_SPLIT(mp->ma_keys);
            if (shared->tracked) {
                shared->marked = 1;
                arg->split_keys_marked++;
            }
        }
    }
    if (!_PyGC_TRACKED(gc)) {
        return true;
    }

    assert(_PyGC_TRACKED(gc));

    if (PyTuple_CheckExact(op)) {
        _PyTuple_MaybeUntrack(op);
        if (!_PyObject_GC_IS_TRACKED(op)) {
            gc->_gc_prev &= ~_PyGC_PREV_MASK_FINALIZED;
            return true;
        }
    }
    else if (PyDict_CheckExact(op)) {
        _PyDict_MaybeUntrack(op);
        if (!_PyObject_GC_IS_TRACKED(op)) {
            gc->_gc_prev &= ~_PyGC_PREV_MASK_FINALIZED;
            return true;
        }
    }

    if (arg->gc_reason == GC_REASON_SHUTDOWN) {
        if (_PyObject_HasDeferredRefcount(op)) {
            // Disable deferred reference counting when we're shutting down.
            // This is useful for interp->sysdict because the last reference
            // to it is cleared after the last GC cycle.
            merge_refcount(op, 0);
        }
    }

    // Add the actual refcount to gc_refs.
    Py_ssize_t refcount = _Py_GC_REFCNT(op);
    _PyObject_ASSERT(op, refcount >= 0);
    if (!(op->ob_gc_bits & _PyGC_UNREACHABLE)) {
        op->ob_tid = 0;
        op->ob_gc_bits = _PyGC_UNREACHABLE;
    }
    gc_add_refs(gc, refcount);

    // Subtract internal references from gc_refs. Objects with gc_refs > 0
    // are directly reachable from outside containers, and so can't be
    // collected.
    Py_TYPE(op)->tp_traverse(op, visit_decref, NULL);
    return true;
}

static void
find_gc_roots(GCState *gcstate, _PyGC_Reason reason, Py_ssize_t *split_keys_marked)
{
    struct update_refs_args args = {
        .gcstate = gcstate,
        .split_keys_marked = 0,
        .gc_reason = reason,
    };
    visit_heaps2(mi_heap_tag_gc, update_refs, &args);
    *split_keys_marked = args.split_keys_marked;
}

/* A traversal callback for subtract_refs. */
static int
visit_decref_unreachable(PyObject *op, void *data)
{
    assert(op != NULL);
    if (PyObject_IS_GC(op)) {
        PyGC_Head *gc = AS_GC(op);
        /* We're only interested in gc_refs for objects in the
         * generation being collected, which can be recognized
         * because only they have positive gc_refs.
         */
        if (gc_is_unreachable2(op)) {
            gc_decref(gc);
        }
    }
    return 0;
}

/* Return true if object has a pre-PEP 442 finalization method. */
static int
has_legacy_finalizer(PyObject *op)
{
    return Py_TYPE(op)->tp_del != NULL;
}

static inline void
clear_unreachable_mask(PyGC_Head *unreachable)
{
    /* Check that the list head does not have the unreachable bit set */
    PyGC_Head *gc, *next;
    for (gc = GC_NEXT(unreachable); gc != unreachable; gc = next) {
        gc->_gc_prev &= ~_PyGC_PREV_MASK_UNREACHABLE;
        next = (PyGC_Head*)gc->_gc_next;
    }
    // validate_list(unreachable, unreachable_clear);
}

/* Adds one to the refcount and merges the local and shared fields. */
static void
incref_merge(PyObject *op)
{
    merge_refcount(op, 1);
}

static void
debug_cycle(const char *msg, PyObject *op)
{
    PySys_FormatStderr("gc: %s <%s %p>\n",
                       msg, Py_TYPE(op)->tp_name, op);
}

/* Clear all weakrefs to unreachable objects, and if such a weakref has a
 * callback, invoke it if necessary.  Note that it's possible for such
 * weakrefs to be outside the unreachable set -- indeed, those are precisely
 * the weakrefs whose callbacks must be invoked.  See gc_weakref.txt for
 * overview & some details.  Some weakrefs with callbacks may be reclaimed
 * directly by this routine; the number reclaimed is the return value.  Other
 * weakrefs with callbacks may be moved into the `old` generation.  Objects
 * moved into `old` have gc_refs set to GC_REACHABLE; the objects remaining in
 * unreachable are left at GC_TENTATIVELY_UNREACHABLE.  When this returns,
 * no object in `unreachable` is weakly referenced anymore.
 */
static void
clear_weakrefs(GCState *gcstate)
{
    /* Clear all weakrefs to the objects in unreachable.  If such a weakref
     * also has a callback, move it into `wrcb_to_call` if the callback
     * needs to be invoked.  Note that we cannot invoke any callbacks until
     * all weakrefs to unreachable objects are cleared, lest the callback
     * resurrect an unreachable object via a still-active weakref.  We
     * make another pass over wrcb_to_call, invoking callbacks, after this
     * pass completes.
     */
    _PyObjectQueue *q = gcstate->gc_unreachable;
    while (q != NULL) {
        for (Py_ssize_t i = 0, n = q->n; i != n; i++) {
            PyObject *op = q->objs[i];

            /* Add one to the refcount to prevent deallocation while we're holding
            * on to it in a list. */
            incref_merge(op);

            /* Print debugging information. */
            if (gcstate->debug & DEBUG_COLLECTABLE) {
                debug_cycle("collectable", op);
            }

            if (PyWeakref_Check(op)) {
                /* A weakref inside the unreachable set must be cleared.  If we
                * allow its callback to execute inside delete_garbage(), it
                * could expose objects that have tp_clear already called on
                * them.  Or, it could resurrect unreachable objects.  One way
                * this can happen is if some container objects do not implement
                * tp_traverse.  Then, wr_object can be outside the unreachable
                * set but can be deallocated as a result of breaking the
                * reference cycle.  If we don't clear the weakref, the callback
                * will run and potentially cause a crash.  See bpo-38006 for
                * one example.
                */
                _PyWeakref_DetachRef((PyWeakReference *)op);
            }

            if (! _PyType_SUPPORTS_WEAKREFS(Py_TYPE(op)))
                continue;

            /* It supports weakrefs.  Does it have any?
            *
            * This is never triggered for static types so we can avoid the
            * (slightly) more costly _PyObject_GET_WEAKREFS_LISTPTR().
            */
            PyWeakrefBase *ctrl = (PyWeakrefBase *)_PyObject_GET_WEAKREF_CONTROL(op);

            if (!ctrl)
                continue;

            PyWeakrefBase *ref;
            for (ref = ctrl->wr_next; ref != ctrl; ref = ref->wr_next) {
                PyWeakReference *wr = (PyWeakReference *)ref;

                if (wr->wr_callback == NULL) {
                    /* no callback */
                    continue;
                }

                /* Headache time.  `op` is going away, and is weakly referenced by
                * `wr`, which has a callback.  Should the callback be invoked?  If wr
                * is also trash, no:
                *
                * 1. There's no need to call it.  The object and the weakref are
                *    both going away, so it's legitimate to pretend the weakref is
                *    going away first.  The user has to ensure a weakref outlives its
                *    referent if they want a guarantee that the wr callback will get
                *    invoked.
                *
                * 2. It may be catastrophic to call it.  If the callback is also in
                *    cyclic trash (CT), then although the CT is unreachable from
                *    outside the current generation, CT may be reachable from the
                *    callback.  Then the callback could resurrect insane objects.
                *
                * Since the callback is never needed and may be unsafe in this case,
                * wr is simply left in the unreachable set.  Note that because we
                * already called _PyWeakref_ClearRef(wr), its callback will never
                * trigger.
                *
                * OTOH, if wr isn't part of CT, we should invoke the callback:  the
                * weakref outlived the trash.  Note that since wr isn't CT in this
                * case, its callback can't be CT either -- wr acted as an external
                * root to this generation, and therefore its callback did too.  So
                * nothing in CT is reachable from the callback either, so it's hard
                * to imagine how calling it later could create a problem for us.  wr
                * is moved to wrcb_to_call in this case.
                */
                if (gc_is_unreachable2((PyObject *)wr)) {
                    /* it should already have been cleared above */
                    // assert(wr->wr_object == Py_None);
                    continue;
                }

                /* Create a new reference so that wr can't go away
                * before we can process it again.
                */
                Py_INCREF(wr);
                _PyObjectQueue_Push(&gcstate->gc_wrcb_to_call, (PyObject *)wr);
            }

            /* Clear the root weakref but does not invoke any callbacks.
            * Other weak references reference this object
            */
            _PyObject_ClearWeakRefsFromGC(op);
        }
        q = q->prev;
    }
}

static void
call_weakref_callbacks(GCState *gcstate)
{
    /* Invoke the callbacks we decided to honor.  It's safe to invoke them
     * because they can't reference unreachable objects.
     */
    PyObject *op;
    while ((op = _PyObjectQueue_Pop(&gcstate->gc_wrcb_to_call))) {
        _PyObject_ASSERT(op, PyWeakref_Check(op));
        PyWeakReference *wr = (PyWeakReference *)op;
        PyObject *callback = wr->wr_callback;
        _PyObject_ASSERT(op, callback != NULL);

        /* copy-paste of weakrefobject.c's handle_callback() */
        PyObject *temp = PyObject_CallOneArg(callback, (PyObject *)wr);
        if (temp == NULL)
            PyErr_WriteUnraisable(callback);
        else
            Py_DECREF(temp);

        /* Give up the reference we created in the first pass.  When
         * op's refcount hits 0 (which it may or may not do right now),
         * op's tp_dealloc will decref op->wr_callback too.  Note
         * that the refcount probably will hit 0 now, and because this
         * weakref was reachable to begin with, gc didn't already
         * add it to its count of freed objects.  Example:  a reachable
         * weak value dict maps some key to this reachable weakref.
         * The callback removes this key->weakref mapping from the
         * dict, leaving no other references to the weakref (excepting
         * ours).
         */
        Py_DECREF(op);
    }
}

static void
merge_queued_objects(_PyObjectQueue **to_dealloc_ptr)
{
    HEAD_LOCK(&_PyRuntime);
    PyThreadState *t;
    for_each_thread(t) {
        _Py_queue_process_gc(t, to_dealloc_ptr);
    }
    HEAD_UNLOCK(&_PyRuntime);
}

static void
dealloc_non_gc(_PyObjectQueue **queue_ptr)
{
    for (;;) {
        PyObject *op = _PyObjectQueue_Pop(queue_ptr);
        if (op == NULL) {
            break;
        }

        _Py_Dealloc(op);
    }
    assert(*queue_ptr == NULL);
}

static void
free_dict_keys(_PyObjectQueue **queue_ptr)
{
    for (;;) {
        PyDictSharedKeysObject *keys = (PyDictSharedKeysObject *)_PyObjectQueue_Pop(queue_ptr);
        if (keys == NULL) {
            break;
        }
        PyMem_Free(keys);
    }
    assert(*queue_ptr == NULL);
}

/* Run first-time finalizers (if any) on all the objects in collectable.
 * Note that this may remove some (or even all) of the objects from the
 * list, due to refcounts falling to 0.
 */
static void
finalize_garbage(PyThreadState *tstate, GCState *gcstate)
{
    _PyObjectQueue *q = gcstate->gc_unreachable;
    while (q != NULL) {
        for (Py_ssize_t i = 0, n = q->n; i != n; i++) {
            PyObject *op = q->objs[i];
            PyGC_Head *gc = AS_GC(op);
            destructor finalize;

            if (!_PyGCHead_FINALIZED(gc) &&
                    (finalize = Py_TYPE(op)->tp_finalize) != NULL) {
                _PyGCHead_SET_FINALIZED(gc);
                finalize(op);
                assert(!_PyErr_Occurred(tstate));
            }
        }
        q = q->prev;
    }
}

/* Break reference cycles by clearing the containers involved.  This is
 * tricky business as the lists can be changing and we don't know which
 * objects may be freed.  It is possible I screwed something up here.
 */
static void
delete_garbage(PyThreadState *tstate, GCState *gcstate)
{
    assert(!_PyErr_Occurred(tstate));

    PyObject *op;
    while ((op = _PyObjectQueue_Pop(&gcstate->gc_unreachable))) {
        if (gc_is_unreachable2(op)) {
            gcstate->gc_collected++;
            op->ob_gc_bits -= _PyGC_UNREACHABLE;

            _PyObject_ASSERT_WITH_MSG(op, _Py_GC_REFCNT(op) > 0,
                                    "refcount is too small");

            if (gcstate->debug & DEBUG_SAVEALL) {
                assert(gcstate->garbage != NULL);
                if (PyList_Append(gcstate->garbage, op) < 0) {
                    _PyErr_Clear(tstate);
                }
            }
            else {
                inquiry clear;
                if ((clear = Py_TYPE(op)->tp_clear) != NULL) {
                    (void) clear(op);
                    if (_PyErr_Occurred(tstate)) {
                        _PyErr_WriteUnraisableMsg("in tp_clear of",
                                                (PyObject*)Py_TYPE(op));
                    }
                }
            }
        }
        assert(op->ob_gc_bits == 0);
        Py_DECREF(op);
    }
}

static void
clear_freelists(PyThreadState *tstate)
{
    _PyTuple_ClearFreeList(tstate);
    _PyFloat_ClearFreeList(tstate);
    _PyList_ClearFreeList(tstate);
    _PyDict_ClearFreeList(tstate);
    _PyAsyncGen_ClearFreeLists(tstate);
    _PyContext_ClearFreeList(tstate);
}

/* Clear all free lists
 * All free lists are cleared during the collection of the highest generation.
 * Allocated items in the free list may keep a pymalloc arena occupied.
 * Clearing the free lists may give back memory to the OS earlier.
 */
static void
clear_all_freelists(PyInterpreterState *interp)
{
    HEAD_LOCK(&_PyRuntime);
    PyThreadState *tstate = interp->threads.head;
    while (tstate != NULL) {
        clear_freelists(tstate);
        tstate = tstate->next;
    }
    HEAD_UNLOCK(&_PyRuntime);
}

static int
visit_reachable_heap(PyObject *op, GCState *gcstate)
{
    if (_PyObject_IS_GC(op) && _PyObject_GC_IS_TRACKED(op)) {
        if (gc_is_unreachable2(op)) {
            op->ob_gc_bits -= _PyGC_UNREACHABLE;
            PyGC_Head *gc = AS_GC(op);
            gc->_gc_prev &= ~_PyGC_PREV_MASK;
            _PyObjectQueue_Push(&gcstate->gc_work, op);
        }
    }
   return 0;
}

static bool
mark_heap_visitor(const mi_heap_t* heap, const mi_heap_area_t* area, void* block, size_t block_size, void* args)
{
    PyGC_Head *gc = (PyGC_Head *)block;
    if (!gc) return true;

    GCState *gcstate = (GCState *)args;

    if (!_PyGC_TRACKED(gc)) {
        return true;
    }

    if (!gc_get_refs(gc)) {
        return true;
    }

    PyObject *op = FROM_GC(gc);
    if (!(gc_is_unreachable2(op))) {
        return true;
    }

    /* gc is definitely reachable from outside the
        * original 'young'.  Mark it as such, and traverse
        * its pointers to find any other objects that may
        * be directly reachable from it.  Note that the
        * call to tp_traverse may append objects to young,
        * so we have to wait until it returns to determine
        * the next object to visit.
        */
    _PyObject_ASSERT_WITH_MSG(op, gc_get_refs(gc) > 0,
                                    "refcount is too small");
    op->ob_gc_bits -= _PyGC_UNREACHABLE;
    gc->_gc_prev &= ~_PyGC_PREV_MASK;
    do {
        traverseproc traverse = Py_TYPE(op)->tp_traverse;
        (void) traverse(op,
                (visitproc)visit_reachable_heap,
                gcstate);
        op = _PyObjectQueue_Pop(&gcstate->gc_work);
    } while (op != NULL);
    return true;
}

static void
restore_tid(mi_segment_t *segment, PyObject *op)
{
    if (_Py_REF_IS_MERGED(op->ob_ref_shared)) {
        op->ob_tid = 0;
    }
    else if (segment->thread_id == 0) {
        merge_refcount(op, 0);
    }
    else {
        // NOTE: may change ob_tid
        op->ob_tid = segment->thread_id;
    }
}

static bool
scan_heap_visitor(const mi_heap_t* heap, const mi_heap_area_t* area, void* block, size_t block_size, void* args)
{
    PyGC_Head *gc = (PyGC_Head *)block;
    if (!gc || !_PyGC_TRACKED(gc)) return true;

    GCState *gcstate = (GCState *)args;
    PyObject *op = FROM_GC(gc);

    restore_tid(_mi_ptr_segment(block), op);

    if (!(op->ob_gc_bits & _PyGC_UNREACHABLE)) {
        // reachable
        gcstate->long_lived_total++;
    }
    else if (has_legacy_finalizer(op)) {
        // would be unreachable, but has legacy finalizer
        op->ob_gc_bits -= _PyGC_UNREACHABLE;
        gcstate->gc_uncollectable++;

        if (gcstate->debug & DEBUG_UNCOLLECTABLE) {
            debug_cycle("uncollectable", op);
        }

       /* Append instances in the uncollectable set to a Python
        * reachable list of garbage.  The programmer has to deal with
        * this if they insist on creating this type of structure.
        */
        if (_PyList_AppendPrivate(gcstate->garbage, op) < 0) {
            PyErr_Clear();
        }
    }
    else {
        // unreachable normal object
        _PyObjectQueue_Push(&gcstate->gc_unreachable, op);
    }
    return true;
}

static inline void
deduce_unreachable_heap(GCState *gcstate) {
    visit_heaps2(mi_heap_tag_gc, mark_heap_visitor, gcstate);

    visit_heaps2(mi_heap_tag_gc, scan_heap_visitor, gcstate);

    // reverse the unreachable queue ordering to better match
    // the order in which objects are allocated (not guaranteed!)
    _PyObjectQueue *prev = NULL;
    _PyObjectQueue *cur = gcstate->gc_unreachable;
    while (cur) {
        _PyObjectQueue *next = cur->prev;
        cur->prev = prev;
        prev = cur;
        cur = next;
    }
    gcstate->gc_unreachable = prev;

    /* Clear weakrefs and enqueue callbacks. */
    clear_weakrefs(gcstate);
}

/* Handle objects that may have resurrected after a call to 'finalize_garbage', moving
   them to 'old_generation' and placing the rest on 'still_unreachable'.

   Contracts:
       * After this function 'unreachable' must not be used anymore and 'still_unreachable'
         will contain the objects that did not resurrect.

       * The "still_unreachable" list must be uninitialized (this function calls
         gc_list_init over 'still_unreachable').

IMPORTANT: After a call to this function, the 'still_unreachable' set will have the
PREV_MARK_COLLECTING set, but the objects in this set are going to be removed so
we can skip the expense of clearing the flag to avoid extra iteration. */
static inline void
handle_resurrected_objects(GCState *gcstate, PyGC_Head *unreachable, PyGC_Head* still_unreachable)
{
    validate_list(unreachable, unreachable_set);
    _PyObjectQueue *q;

    q = gcstate->gc_unreachable;
    while (q != NULL) {
        for (Py_ssize_t i = 0, n = q->n; i != n; i++) {
            PyObject *op = q->objs[i];
            PyGC_Head *gc = AS_GC(op);
            assert(gc_get_refs(gc) == 0);
        }
        q = q->prev;
    }

    // First reset the reference count for unreachable objects. Subtract one
    // from the reference count to account for the refcount increment due
    // to being in the "unreachable" list.
    q = gcstate->gc_unreachable;
    while (q != NULL) {
        for (Py_ssize_t i = 0, n = q->n; i != n; i++) {
            PyObject *op = q->objs[i];
            assert(gc_is_unreachable2(op));
            
            Py_ssize_t refcnt = _Py_GC_REFCNT(op);
            _PyObject_ASSERT(op, refcnt > 0);
            gc_add_refs(AS_GC(op), refcnt - 1);

            traverseproc traverse = Py_TYPE(op)->tp_traverse;
            (void) traverse(op,
                        (visitproc)visit_decref_unreachable,
                        NULL);
        }
        q = q->prev;
    }

    // Find any resurrected objects
    q = gcstate->gc_unreachable;
    while (q != NULL) {
        for (Py_ssize_t i = 0, n = q->n; i != n; i++) {
            PyObject *op = q->objs[i];
            PyGC_Head *gc = AS_GC(op);
            const Py_ssize_t gc_refs = gc_get_refs(gc);
            assert(gc_refs >= 0);
            restore_tid(_mi_ptr_segment(op), op);
            if (gc_refs == 0 || !gc_is_unreachable2(op)) {
                continue;
            }
            op->ob_gc_bits -= _PyGC_UNREACHABLE;
            gc->_gc_prev &= ~_PyGC_PREV_MASK;
            do {
                traverseproc traverse = Py_TYPE(op)->tp_traverse;
                (void) traverse(op,
                        (visitproc)visit_reachable_heap,
                        gcstate);
                op = _PyObjectQueue_Pop(&gcstate->gc_work);
            } while (op != NULL);
        }
        q = q->prev;
    }
}

static void
update_gc_threshold(GCState *gcstate)
{
    Py_ssize_t live = _Py_atomic_load_ssize(&gcstate->gc_live);
    Py_ssize_t threshold = live + (live * gcstate->gc_scale) / 100;
    if (threshold < 7000) {
        threshold = 7000;
    }
    _Py_atomic_store_ssize(&gcstate->gc_threshold, threshold);
}

static int
gc_reason_is_valid(GCState *gcstate, _PyGC_Reason reason)
{
    if (reason == GC_REASON_HEAP) {
        return _PyGC_ShouldCollect(gcstate);
    }
    return 1;
}

static void
invoke_gc_callback(PyThreadState *tstate, const char *phase,
                   Py_ssize_t collected, Py_ssize_t uncollectable);

/* This is the main function.  Read this to understand how the
 * collection process works. */
static Py_ssize_t
gc_collect_main(PyThreadState *tstate, int generation, _PyGC_Reason reason)
{
    PyGC_Head unreachable; /* non-problematic unreachable trash */
    _PyObjectQueue *to_dealloc = NULL;
    _PyTime_t t1 = 0;   /* initialize to prevent a compiler warning */
    GCState *gcstate = &tstate->interp->gc;

    gcstate->gc_collected = 0; /* # objects collected */
    gcstate->gc_uncollectable = 0; /* # unreachable objects that couldn't be collected */
    gcstate->long_lived_pending = 0;
    gcstate->long_lived_total = 0;

    // gc_collect_main() must not be called before _PyGC_Init
    // or after _PyGC_Fini()
    assert(gcstate->garbage != NULL);
    assert(!_PyErr_Occurred(tstate));

    if (tstate->cant_stop_wont_stop) {
        // Don't start a garbage collection if this thread is in a critical
        // section that doesn't allow GC.
        return 0;
    }

    if (!_Py_atomic_compare_exchange_int(&_PyRuntime.gc_collecting, 0, 1)) {
        // Don't start a garbage collection if a collection is already in
        // progress.
        return 0;
    }

    if (!gc_reason_is_valid(gcstate, reason)) {
        _Py_atomic_store_int(&_PyRuntime.gc_collecting, 0);
        return 0;
    }

    _Py_atomic_store_int(&gcstate->collecting, 1);

    _PyRuntimeState_StopTheWorld(&_PyRuntime);

    if (reason != GC_REASON_SHUTDOWN) {
        invoke_gc_callback(tstate, "start", 0, 0);
    }

    if (gcstate->debug & DEBUG_STATS) {
        PySys_WriteStderr("gc: collecting heap...\n");
        PySys_FormatStderr(
            "gc: live objects: %"PY_FORMAT_SIZE_T"d\n",
            gcstate->gc_live);
        t1 = _PyTime_GetMonotonicClock();
    }

    if (PyDTrace_GC_START_ENABLED())
        PyDTrace_GC_START(NUM_GENERATIONS - 1);

    /* Merge the refcount for all queued objects, but do not dealloc
     * yet. Objects with zero refcount that are tracked will be freed during
     * GC. Non-tracked objects are added to "to_dealloc" and freed once
     * threads are resumed.
     */
    merge_queued_objects(&to_dealloc);
    validate_tracked_heap(_PyGC_PREV_MASK|_PyGC_PREV_MASK_UNREACHABLE, 0);

    Py_ssize_t split_keys_marked = 0;
    find_gc_roots(gcstate, reason, &split_keys_marked);

    _PyObjectQueue *dead_keys = NULL;
    int split_keys_unmarked = 0;
    find_dead_shared_keys(&dead_keys, &split_keys_unmarked);
    free_dict_keys(&dead_keys);
    assert(split_keys_marked == split_keys_unmarked);

    deduce_unreachable_heap(gcstate);

    /* Restart the world to call weakrefs and finalizers */
    _PyRuntimeState_StartTheWorld(&_PyRuntime);

    /* Dealloc objects with zero refcount that are not tracked by GC */
    dealloc_non_gc(&to_dealloc);

    call_weakref_callbacks(gcstate);

    /* Call tp_finalize on objects which have one. */
    finalize_garbage(tstate, gcstate);

    _PyRuntimeState_StopTheWorld(&_PyRuntime);

    validate_refcount();

    /* Handle any objects that may have resurrected after the call
     * to 'finalize_garbage' and continue the collection with the
     * objects that are still unreachable */
    gc_list_init(&unreachable);
    PyGC_Head final_unreachable;
    gc_list_init(&final_unreachable);
    handle_resurrected_objects(gcstate, &unreachable, &final_unreachable);

    /* Clear free list only during the collection of the highest
     * generation */
    if (generation == NUM_GENERATIONS-1) {
        clear_all_freelists(tstate->interp);
    }

    _PyRuntimeState_StartTheWorld(&_PyRuntime);

    /* Call tp_clear on objects in the final_unreachable set.  This will cause
    * the reference cycles to be broken.  It may also cause some objects
    * in finalizers to be freed.
    */
    // gcstate->gc_collected += gc_list_size(&final_unreachable);
    delete_garbage(tstate, gcstate);

    if (reason == GC_REASON_MANUAL) {
        // Clear this thread's freelists again after deleting garbage
        // for more precise block accounting when calling gc.collect().
        clear_freelists(tstate);
    }

    if (gcstate->debug & DEBUG_STATS) {
        double d = _PyTime_AsSecondsDouble(_PyTime_GetPerfCounter() - t1);
        PySys_WriteStderr(
            "gc: done, %zd unreachable, %zd uncollectable, %.4fs elapsed\n",
            gcstate->gc_collected + gcstate->gc_uncollectable,
            gcstate->gc_uncollectable,
            d);
    }

    _Py_qsbr_advance(&_PyRuntime.qsbr_shared);
    _Py_qsbr_quiescent_state(tstate);
    _PyMem_QsbrPoll(tstate);

    if (_PyErr_Occurred(tstate)) {
        if (reason == GC_REASON_SHUTDOWN) {
            _PyErr_Clear(tstate);
        }
        else {
            _PyErr_WriteUnraisableMsg("in garbage collection", NULL);
        }
    }

    /* Update stats */
    struct gc_generation_stats *stats = &gcstate->stats;
    stats->collections++;
    stats->collected += gcstate->gc_collected;
    stats->uncollectable += gcstate->gc_uncollectable;
    Py_ssize_t num_unreachable = gcstate->gc_collected + gcstate->gc_uncollectable;

    update_gc_threshold(gcstate);

    if (PyDTrace_GC_DONE_ENABLED()) {
        PyDTrace_GC_DONE(num_unreachable);
    }

    assert(!_PyErr_Occurred(tstate));

    if (reason != GC_REASON_SHUTDOWN) {
        invoke_gc_callback(tstate, "stop", gcstate->gc_collected, gcstate->gc_uncollectable);
    }

    _Py_atomic_store_int(&gcstate->collecting, 0);
    _Py_atomic_store_int(&_PyRuntime.gc_collecting, 0);
    return num_unreachable;
}

/* Invoke progress callbacks to notify clients that garbage collection
 * is starting or stopping
 */
static void
invoke_gc_callback(PyThreadState *tstate, const char *phase,
                   Py_ssize_t collected, Py_ssize_t uncollectable)
{
    assert(!_PyErr_Occurred(tstate));

    /* we may get called very early */
    GCState *gcstate = &tstate->interp->gc;
    if (gcstate->callbacks == NULL) {
        return;
    }

    /* The local variable cannot be rebound, check it for sanity */
    assert(PyList_CheckExact(gcstate->callbacks));
    PyObject *info = NULL;
    if (PyList_GET_SIZE(gcstate->callbacks) != 0) {
        info = Py_BuildValue("{sisnsn}",
            "generation", 0,    // what value maximizes compatiblity?
            "collected", collected,
            "uncollectable", uncollectable);
        if (info == NULL) {
            PyErr_WriteUnraisable(NULL);
            return;
        }
    }
    for (Py_ssize_t i=0; i<PyList_GET_SIZE(gcstate->callbacks); i++) {
        PyObject *r, *cb = PyList_GET_ITEM(gcstate->callbacks, i);
        Py_INCREF(cb); /* make sure cb doesn't go away */
        r = PyObject_CallFunction(cb, "sO", phase, info);
        if (r == NULL) {
            PyErr_WriteUnraisable(cb);
        }
        else {
            Py_DECREF(r);
        }
        Py_DECREF(cb);
    }
    Py_XDECREF(info);
    assert(!_PyErr_Occurred(tstate));
}

#include "clinic/gcmodule.c.h"

/*[clinic input]
gc.enable

Enable automatic garbage collection.
[clinic start generated code]*/

static PyObject *
gc_enable_impl(PyObject *module)
/*[clinic end generated code: output=45a427e9dce9155c input=81ac4940ca579707]*/
{
    PyGC_Enable();
    Py_RETURN_NONE;
}

/*[clinic input]
gc.disable

Disable automatic garbage collection.
[clinic start generated code]*/

static PyObject *
gc_disable_impl(PyObject *module)
/*[clinic end generated code: output=97d1030f7aa9d279 input=8c2e5a14e800d83b]*/
{
    PyGC_Disable();
    Py_RETURN_NONE;
}

/*[clinic input]
gc.isenabled -> bool

Returns true if automatic garbage collection is enabled.
[clinic start generated code]*/

static int
gc_isenabled_impl(PyObject *module)
/*[clinic end generated code: output=1874298331c49130 input=30005e0422373b31]*/
{
    return PyGC_IsEnabled();
}

/*[clinic input]
gc.collect -> Py_ssize_t

    generation: int(c_default="NUM_GENERATIONS - 1") = 2

Run the garbage collector.

With no arguments, run a full collection.  The optional argument
may be an integer specifying which generation to collect.  A ValueError
is raised if the generation number is invalid.

The number of unreachable objects is returned.
[clinic start generated code]*/

static Py_ssize_t
gc_collect_impl(PyObject *module, int generation)
/*[clinic end generated code: output=b697e633043233c7 input=40720128b682d879]*/
{
    PyThreadState *tstate = _PyThreadState_GET();

    if (generation < 0 || generation >= 3) {
        _PyErr_SetString(tstate, PyExc_ValueError, "invalid generation");
        return -1;
    }

    return gc_collect_main(tstate, generation, GC_REASON_MANUAL);
}

/*[clinic input]
gc.set_debug

    flags: int
        An integer that can have the following bits turned on:
          DEBUG_STATS - Print statistics during collection.
          DEBUG_COLLECTABLE - Print collectable objects found.
          DEBUG_UNCOLLECTABLE - Print unreachable but uncollectable objects
            found.
          DEBUG_SAVEALL - Save objects to gc.garbage rather than freeing them.
          DEBUG_LEAK - Debug leaking programs (everything but STATS).
    /

Set the garbage collection debugging flags.

Debugging information is written to sys.stderr.
[clinic start generated code]*/

static PyObject *
gc_set_debug_impl(PyObject *module, int flags)
/*[clinic end generated code: output=7c8366575486b228 input=5e5ce15e84fbed15]*/
{
    GCState *gcstate = get_gc_state();
    gcstate->debug = flags;
    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_debug -> int

Get the garbage collection debugging flags.
[clinic start generated code]*/

static int
gc_get_debug_impl(PyObject *module)
/*[clinic end generated code: output=91242f3506cd1e50 input=91a101e1c3b98366]*/
{
    GCState *gcstate = get_gc_state();
    return gcstate->debug;
}

PyDoc_STRVAR(gc_set_thresh__doc__,
"set_threshold(threshold0, [threshold1, threshold2]) -> None\n"
"\n"
"Sets the collection thresholds.  Setting threshold0 to zero disables\n"
"collection.\n");

static PyObject *
gc_set_threshold(PyObject *self, PyObject *args)
{
    GCState *gcstate = get_gc_state();
    int threshold0, threshold1, threshold2;

    if (!PyArg_ParseTuple(args, "i|ii:set_threshold",
                          &threshold0,
                          &threshold1,
                          &threshold2))
        return NULL;

    // FIXME: does setting threshold0 to zero actually disable collection ???
    gcstate->gc_threshold = threshold0;
    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_threshold

Return the current collection thresholds.
[clinic start generated code]*/

static PyObject *
gc_get_threshold_impl(PyObject *module)
/*[clinic end generated code: output=7902bc9f41ecbbd8 input=286d79918034d6e6]*/
{
    GCState *gcstate = get_gc_state();
    return Py_BuildValue("(nii)",
                         gcstate->gc_threshold,
                         0,
                         0);
}

/*[clinic input]
gc.get_count

Return a three-tuple of the current collection counts.
[clinic start generated code]*/

static PyObject *
gc_get_count_impl(PyObject *module)
/*[clinic end generated code: output=354012e67b16398f input=a392794a08251751]*/
{
    GCState *gcstate = get_gc_state();
    Py_ssize_t gc_live = _Py_atomic_load_ssize(&gcstate->gc_live);
    return Py_BuildValue("(nii)", gc_live, 0, 0);
}

static int
referrersvisit(PyObject* obj, PyObject *objs)
{
    Py_ssize_t i;
    for (i = 0; i < PyTuple_GET_SIZE(objs); i++)
        if (PyTuple_GET_ITEM(objs, i) == obj)
            return 1;
    return 0;
}

struct gc_referrers_arg {
    PyObject *objs;
    PyObject *resultlist;
};

static int
gc_referrers_visitor(PyGC_Head *gc, void *void_arg)
{
    struct gc_referrers_arg *arg = (struct gc_referrers_arg*)void_arg;
    PyObject *objs = arg->objs;
    PyObject *resultlist = arg->resultlist;

    PyObject *obj = FROM_GC(gc);
    traverseproc traverse = Py_TYPE(obj)->tp_traverse;
    if (obj == objs || obj == resultlist) {
        return 0;
    }
    if (traverse(obj, (visitproc)referrersvisit, objs)) {
        if (PyList_Append(resultlist, obj) < 0) {
            return -1; /* error */
        }
    }
    return 0;
}

PyDoc_STRVAR(gc_get_referrers__doc__,
"get_referrers(*objs) -> list\n\
Return the list of objects that directly refer to any of objs.");

static PyObject *
gc_get_referrers(PyObject *self, PyObject *args)
{
    if (PySys_Audit("gc.get_referrers", "(O)", args) < 0) {
        return NULL;
    }

    PyObject *result = PyList_New(0);
    if (!result) {
        return NULL;
    }

    struct gc_referrers_arg arg;
    arg.objs = args;
    arg.resultlist = result;
    if (visit_heaps(gc_referrers_visitor, &arg) < 0) {
        Py_DECREF(result);
        return NULL;
    }
    return result;
}

/* Append obj to list; return true if error (out of memory), false if OK. */
static int
referentsvisit(PyObject *obj, PyObject *list)
{
    return PyList_Append(list, obj) < 0;
}

PyDoc_STRVAR(gc_get_referents__doc__,
"get_referents(*objs) -> list\n\
Return the list of objects that are directly referred to by objs.");

static PyObject *
gc_get_referents(PyObject *self, PyObject *args)
{
    Py_ssize_t i;
    if (PySys_Audit("gc.get_referents", "(O)", args) < 0) {
        return NULL;
    }
    PyObject *result = PyList_New(0);

    if (result == NULL)
        return NULL;

    for (i = 0; i < PyTuple_GET_SIZE(args); i++) {
        traverseproc traverse;
        PyObject *obj = PyTuple_GET_ITEM(args, i);

        if (!_PyObject_IS_GC(obj))
            continue;
        traverse = Py_TYPE(obj)->tp_traverse;
        if (! traverse)
            continue;
        if (traverse(obj, (visitproc)referentsvisit, result)) {
            Py_DECREF(result);
            return NULL;
        }
    }
    return result;
}

struct gc_get_objects_arg {
    PyObject *py_list;
    Py_ssize_t generation;
};

static int
gc_get_objects_visitor(PyGC_Head *gc, void *void_arg)
{
    PyObject *op = FROM_GC(gc);

    struct gc_get_objects_arg *arg = (struct gc_get_objects_arg*)void_arg;
    PyObject *py_list = arg->py_list;

    if (op == py_list) {
        return 0;
    }
    if (PyList_Append(py_list, op)) {
        return -1;
    }
    return 0;
}

/*[clinic input]
gc.get_objects
    generation: Py_ssize_t(accept={int, NoneType}, c_default="-1") = None
        Generation to extract the objects from.

Return a list of objects tracked by the collector (excluding the list returned).

If generation is not None, return only the objects tracked by the collector
that are in that generation.
[clinic start generated code]*/

static PyObject *
gc_get_objects_impl(PyObject *module, Py_ssize_t generation)
/*[clinic end generated code: output=48b35fea4ba6cb0e input=ef7da9df9806754c]*/
{
    PyObject* result;

    if (PySys_Audit("gc.get_objects", "n", generation) < 0) {
        return NULL;
    }

    result = PyList_New(0);
    if (result == NULL) {
        return NULL;
    }

    if (generation >= NUM_GENERATIONS) {
        PyErr_Format(PyExc_ValueError,
                    "generation parameter must be less than the number of "
                    "available generations (%i)",
                    NUM_GENERATIONS);
        goto error;
    }

    /* If generation is passed, we extract only that generation */
    if (generation < -1) {
        PyErr_SetString(PyExc_ValueError,
                        "generation parameter cannot be negative");
        goto error;
    }

    struct gc_get_objects_arg arg;
    arg.py_list = result;
    arg.generation = generation + 1;
    if (visit_heaps(gc_get_objects_visitor, &arg) < 0) {
        goto error;
    }

    return result;

error:
    Py_DECREF(result);
    return NULL;
}

/*[clinic input]
gc.get_stats

Return a list of dictionaries containing per-generation statistics.
[clinic start generated code]*/

static PyObject *
gc_get_stats_impl(PyObject *module)
/*[clinic end generated code: output=a8ab1d8a5d26f3ab input=1ef4ed9d17b1a470]*/
{
    struct gc_generation_stats stats;
    PyObject *result, *dict;

    /* To get consistent values despite allocations while constructing
       the result list, we use a snapshot of the running stats. */
    stats = get_gc_state()->stats;

    result = PyList_New(0);
    if (result == NULL)
        return NULL;

    dict = Py_BuildValue("{snsnsn}",
                         "collections", stats.collections,
                         "collected", stats.collected,
                         "uncollectable", stats.uncollectable
                        );
    if (dict == NULL)
        goto error;
    if (PyList_Append(result, dict)) {
        Py_DECREF(dict);
        goto error;
    }
    Py_DECREF(dict);
    return result;

error:
    Py_XDECREF(result);
    return NULL;
}


/*[clinic input]
gc.is_tracked

    obj: object
    /

Returns true if the object is tracked by the garbage collector.

Simple atomic objects will return false.
[clinic start generated code]*/

static PyObject *
gc_is_tracked(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=14f0103423b28e31 input=d83057f170ea2723]*/
{
    PyObject *result;

    if (_PyObject_IS_GC(obj) && _PyObject_GC_IS_TRACKED(obj))
        result = Py_True;
    else
        result = Py_False;
    return Py_NewRef(result);
}

/*[clinic input]
gc.is_finalized

    obj: object
    /

Returns true if the object has been already finalized by the GC.
[clinic start generated code]*/

static PyObject *
gc_is_finalized(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=e1516ac119a918ed input=201d0c58f69ae390]*/
{
    if (_PyObject_IS_GC(obj) && _PyGCHead_FINALIZED(AS_GC(obj))) {
         Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

/*[clinic input]
gc.freeze

Freeze all current tracked objects and ignore them for future collections.

This can be used before a POSIX fork() call to make the gc copy-on-write friendly.
Note: collection before a POSIX fork() call may free pages for future allocation
which can cause copy-on-write.
[clinic start generated code]*/

static PyObject *
gc_freeze_impl(PyObject *module)
/*[clinic end generated code: output=502159d9cdc4c139 input=b602b16ac5febbe5]*/
{
    // we only have a single generation, so this doesn't do anything
    // TODO: untrack objects?
    Py_RETURN_NONE;
}

/*[clinic input]
gc.unfreeze

Unfreeze all objects in the permanent generation.

Put all objects in the permanent generation back into oldest generation.
[clinic start generated code]*/

static PyObject *
gc_unfreeze_impl(PyObject *module)
/*[clinic end generated code: output=1c15f2043b25e169 input=2dd52b170f4cef6c]*/
{
    // we only have a single generation, so this doesn't do anything
    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_freeze_count -> Py_ssize_t

Return the number of objects in the permanent generation.
[clinic start generated code]*/

static Py_ssize_t
gc_get_freeze_count_impl(PyObject *module)
/*[clinic end generated code: output=61cbd9f43aa032e1 input=45ffbc65cfe2a6ed]*/
{
    return 0;
}


PyDoc_STRVAR(gc__doc__,
"This module provides access to the garbage collector for reference cycles.\n"
"\n"
"enable() -- Enable automatic garbage collection.\n"
"disable() -- Disable automatic garbage collection.\n"
"isenabled() -- Returns true if automatic collection is enabled.\n"
"collect() -- Do a full collection right now.\n"
"get_count() -- Return the current collection counts.\n"
"get_stats() -- Return list of dictionaries containing per-generation stats.\n"
"set_debug() -- Set debugging flags.\n"
"get_debug() -- Get debugging flags.\n"
"set_threshold() -- Set the collection thresholds.\n"
"get_threshold() -- Return the current the collection thresholds.\n"
"get_objects() -- Return a list of all objects tracked by the collector.\n"
"is_tracked() -- Returns true if a given object is tracked.\n"
"is_finalized() -- Returns true if a given object has been already finalized.\n"
"get_referrers() -- Return the list of objects that refer to an object.\n"
"get_referents() -- Return the list of objects that an object refers to.\n"
"freeze() -- Freeze all tracked objects and ignore them for future collections.\n"
"unfreeze() -- Unfreeze all objects in the permanent generation.\n"
"get_freeze_count() -- Return the number of objects in the permanent generation.\n");

static PyMethodDef GcMethods[] = {
    GC_ENABLE_METHODDEF
    GC_DISABLE_METHODDEF
    GC_ISENABLED_METHODDEF
    GC_SET_DEBUG_METHODDEF
    GC_GET_DEBUG_METHODDEF
    GC_GET_COUNT_METHODDEF
    {"set_threshold",  gc_set_threshold, METH_VARARGS, gc_set_thresh__doc__},
    GC_GET_THRESHOLD_METHODDEF
    GC_COLLECT_METHODDEF
    GC_GET_OBJECTS_METHODDEF
    GC_GET_STATS_METHODDEF
    GC_IS_TRACKED_METHODDEF
    GC_IS_FINALIZED_METHODDEF
    {"get_referrers",  gc_get_referrers, METH_VARARGS,
        gc_get_referrers__doc__},
    {"get_referents",  gc_get_referents, METH_VARARGS,
        gc_get_referents__doc__},
    GC_FREEZE_METHODDEF
    GC_UNFREEZE_METHODDEF
    GC_GET_FREEZE_COUNT_METHODDEF
    {NULL,      NULL}           /* Sentinel */
};

static int
gcmodule_exec(PyObject *module)
{
    GCState *gcstate = get_gc_state();

    /* garbage and callbacks are initialized by _PyGC_Init() early in
     * interpreter lifecycle. */
    assert(gcstate->garbage != NULL);
    if (PyModule_AddObjectRef(module, "garbage", gcstate->garbage) < 0) {
        return -1;
    }
    assert(gcstate->callbacks != NULL);
    if (PyModule_AddObjectRef(module, "callbacks", gcstate->callbacks) < 0) {
        return -1;
    }

#define ADD_INT(NAME) if (PyModule_AddIntConstant(module, #NAME, NAME) < 0) { return -1; }
    ADD_INT(DEBUG_STATS);
    ADD_INT(DEBUG_COLLECTABLE);
    ADD_INT(DEBUG_UNCOLLECTABLE);
    ADD_INT(DEBUG_SAVEALL);
    ADD_INT(DEBUG_LEAK);
#undef ADD_INT
    return 0;
}

static PyModuleDef_Slot gcmodule_slots[] = {
    {Py_mod_exec, gcmodule_exec},
    {0, NULL}
};

static struct PyModuleDef gcmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "gc",
    .m_doc = gc__doc__,
    .m_size = 0,  // per interpreter state, see: get_gc_state()
    .m_methods = GcMethods,
    .m_slots = gcmodule_slots
};

PyMODINIT_FUNC
PyInit_gc(void)
{
    return PyModuleDef_Init(&gcmodule);
}

/* C API for controlling the state of the garbage collector */
int
PyGC_Enable(void)
{
    GCState *gcstate = get_gc_state();
    int old_state = gcstate->enabled;
    gcstate->enabled = 1;
    return old_state;
}

int
PyGC_Disable(void)
{
    GCState *gcstate = get_gc_state();
    int old_state = gcstate->enabled;
    gcstate->enabled = 0;
    return old_state;
}

int
PyGC_IsEnabled(void)
{
    GCState *gcstate = get_gc_state();
    return gcstate->enabled;
}

/* Public API to invoke gc.collect() from C */
Py_ssize_t
PyGC_Collect(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    GCState *gcstate = &tstate->interp->gc;

    if (!gcstate->enabled) {
        return 0;
    }

    return gc_collect_main(tstate, NUM_GENERATIONS - 1, GC_REASON_MANUAL);
}

Py_ssize_t
_PyGC_CollectNoFail(PyThreadState *tstate)
{
    assert(!_PyErr_Occurred(tstate));
    /* Ideally, this function is only called on interpreter shutdown,
       and therefore not recursively.  Unfortunately, when there are daemon
       threads, a daemon thread can start a cyclic garbage collection
       during interpreter shutdown (and then never finish it).
       See http://bugs.python.org/issue8713#msg195178 for an example.
       */
    return gc_collect_main(tstate, NUM_GENERATIONS - 1, GC_REASON_SHUTDOWN);
}

void
_PyGC_DumpShutdownStats(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    if (!(gcstate->debug & DEBUG_SAVEALL)
        && gcstate->garbage != NULL && PyList_GET_SIZE(gcstate->garbage) > 0) {
        const char *message;
        if (gcstate->debug & DEBUG_UNCOLLECTABLE)
            message = "gc: %zd uncollectable objects at " \
                "shutdown";
        else
            message = "gc: %zd uncollectable objects at " \
                "shutdown; use gc.set_debug(gc.DEBUG_UNCOLLECTABLE) to list them";
        /* PyErr_WarnFormat does too many things and we are at shutdown,
           the warnings module's dependencies (e.g. linecache) may be gone
           already. */
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL, message,
                                     PyList_GET_SIZE(gcstate->garbage)))
            PyErr_WriteUnraisable(NULL);
        if (gcstate->debug & DEBUG_UNCOLLECTABLE) {
            PyObject *repr = NULL, *bytes = NULL;
            repr = PyObject_Repr(gcstate->garbage);
            if (!repr || !(bytes = PyUnicode_EncodeFSDefault(repr)))
                PyErr_WriteUnraisable(gcstate->garbage);
            else {
                PySys_WriteStderr(
                    "      %s\n",
                    PyBytes_AS_STRING(bytes)
                    );
            }
            Py_XDECREF(repr);
            Py_XDECREF(bytes);
        }
    }
}

static void
gc_fini_untrack(GCState *gcstate)
{
    // PyGC_Head *gc;
    // for (gc = GC_NEXT(list); gc != list; gc = GC_NEXT(list)) {
    //     PyObject *op = FROM_GC(gc);
    //     _PyObject_GC_UNTRACK(op);
    //     // gh-92036: If a deallocator function expect the object to be tracked
    //     // by the GC (ex: func_dealloc()), it can crash if called on an object
    //     // which is no longer tracked by the GC. Leak one strong reference on
    //     // purpose so the object is never deleted and its deallocator is not
    //     // called.
    //     Py_INCREF(op);
    // }
}


void
_PyGC_Fini(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    Py_CLEAR(gcstate->garbage);
    Py_CLEAR(gcstate->callbacks);

    if (!_Py_IsMainInterpreter(interp)) {
        // bpo-46070: Explicitly untrack all objects currently tracked by the
        // GC. Otherwise, if an object is used later by another interpreter,
        // calling PyObject_GC_UnTrack() on the object crashs if the previous
        // or the next object of the PyGC_Head structure became a dangling
        // pointer.
        gc_fini_untrack(gcstate);
    }
}

/* for debugging */
void
_PyGC_Dump(PyGC_Head *g)
{
    _PyObject_Dump(FROM_GC(g));
}


#ifdef Py_DEBUG
static int
visit_validate(PyObject *op, void *parent_raw)
{
    PyObject *parent = _PyObject_CAST(parent_raw);
    if (_PyObject_IsFreed(op)) {
        _PyObject_ASSERT_FAILED_MSG(parent,
                                    "PyObject_GC_Track() object is not valid");
    }
    return 0;
}
#endif


/* extension modules might be compiled with GC support so these
   functions must always be available */

void
PyObject_GC_Track(void *op_raw)
{
    PyObject *op = _PyObject_CAST(op_raw);
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_ASSERT_FAILED_MSG(op,
                                    "object already tracked "
                                    "by the garbage collector");
    }
    _PyObject_GC_TRACK(op);

#ifdef Py_DEBUG
    /* Check that the object is valid: validate objects traversed
       by tp_traverse() */
    traverseproc traverse = Py_TYPE(op)->tp_traverse;
    (void)traverse(op, visit_validate, op);
#endif
}

void
PyObject_GC_UnTrack(void *op_raw)
{
    PyObject *op = _PyObject_CAST(op_raw);
    /* Obscure:  the Py_TRASHCAN mechanism requires that we be able to
     * call PyObject_GC_UnTrack twice on an object.
     */
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_GC_UNTRACK(op);
    }
}

int
PyObject_IS_GC(PyObject *obj)
{
    return _PyObject_IS_GC(obj);
}

void
_Py_RunGC(PyThreadState *tstate)
{
    gc_collect_main(tstate, 0, GC_REASON_HEAP);
}

static PyObject *
gc_alloc(size_t basicsize, size_t presize)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (basicsize > PY_SSIZE_T_MAX - presize) {
        return _PyErr_NoMemory(tstate);
    }
    size_t size = presize + basicsize;
    PyMemAllocatorEx *a = &_PyRuntime.allocators.standard.gc;
    char *mem = a->malloc(a->ctx, size);
    if (mem == NULL) {
        return _PyErr_NoMemory(tstate);
    }
    memset(mem, 0, presize);
    return (PyObject *)(mem + presize);
}

PyObject *
_PyObject_GC_New(PyTypeObject *tp)
{
    size_t presize = _PyType_PreHeaderSize(tp);
    PyObject *op = gc_alloc(_PyObject_SIZE(tp), presize);
    if (op == NULL) {
        return NULL;
    }
    _PyObject_Init(op, tp);
    return op;
}

PyVarObject *
_PyObject_GC_NewVar(PyTypeObject *tp, Py_ssize_t nitems)
{
    PyVarObject *op;

    if (nitems < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    size_t presize = _PyType_PreHeaderSize(tp);
    size_t size = _PyObject_VAR_SIZE(tp, nitems);
    op = (PyVarObject *)gc_alloc(size, presize);
    if (op == NULL) {
        return NULL;
    }
    _PyObject_InitVar(op, tp, nitems);
    return op;
}

PyVarObject *
_PyObject_GC_Resize(PyVarObject *op, Py_ssize_t nitems)
{
    size_t presize = _PyType_PreHeaderSize(Py_TYPE(op));
    const size_t basicsize = _PyObject_VAR_SIZE(Py_TYPE(op), nitems);
    _PyObject_ASSERT((PyObject *)op, !_PyObject_GC_IS_TRACKED(op));
    if (basicsize > (size_t)PY_SSIZE_T_MAX - presize) {
        return (PyVarObject *)PyErr_NoMemory();
    }

    PyMemAllocatorEx *a = &_PyRuntime.allocators.standard.gc;
    char *mem = (char *)op - presize;
    mem = a->realloc(a->ctx, mem, presize + basicsize);
    if (mem == NULL)
        return (PyVarObject *)PyErr_NoMemory();
    op = (PyVarObject *) (mem + presize);
    Py_SET_SIZE(op, nitems);
    return op;
}

void
PyObject_GC_Del(void *op)
{
    size_t presize = _PyType_PreHeaderSize(((PyObject *)op)->ob_type);
    if (_PyObject_GC_IS_TRACKED(op)) {
#ifdef Py_DEBUG
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL, "Object of type %s is not untracked before destruction",
                                     ((PyObject*)op)->ob_type->tp_name)) {
            PyErr_WriteUnraisable(NULL);
        }
#endif
    }
    PyMemAllocatorEx *a = &_PyRuntime.allocators.standard.gc;
    a->free(a->ctx, ((char *)op)-presize);
}

int
PyObject_GC_IsTracked(PyObject* obj)
{
    if (_PyObject_IS_GC(obj) && _PyObject_GC_IS_TRACKED(obj)) {
        return 1;
    }
    return 0;
}

int
PyObject_GC_IsFinalized(PyObject *obj)
{
    if (_PyObject_IS_GC(obj) && _PyGCHead_FINALIZED(AS_GC(obj))) {
         return 1;
    }
    return 0;
}
