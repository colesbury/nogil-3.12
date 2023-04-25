#ifndef Py_CPYTHON_PYSTATE_H
#  error "this header file must not be included directly"
#endif

/*
Runtime Feature Flags

Each flag indicate whether or not a specific runtime feature
is available in a given context.  For example, forking the process
might not be allowed in the current interpreter (i.e. os.fork() would fail).
*/

/* Set if threads are allowed. */
#define Py_RTFLAGS_THREADS (1UL << 10)

/* Set if daemon threads are allowed. */
#define Py_RTFLAGS_DAEMON_THREADS (1UL << 11)

/* Set if os.fork() is allowed. */
#define Py_RTFLAGS_FORK (1UL << 15)

/* Set if os.exec*() is allowed. */
#define Py_RTFLAGS_EXEC (1UL << 16)


PyAPI_FUNC(int) _PyInterpreterState_HasFeature(PyInterpreterState *interp,
                                               unsigned long feature);


/* private interpreter helpers */

PyAPI_FUNC(int) _PyInterpreterState_RequiresIDRef(PyInterpreterState *);
PyAPI_FUNC(void) _PyInterpreterState_RequireIDRef(PyInterpreterState *, int);

PyAPI_FUNC(PyObject *) _PyInterpreterState_GetMainModule(PyInterpreterState *);


/* State unique per thread */

/* Py_tracefunc return -1 when raising an exception, or 0 for success. */
typedef int (*Py_tracefunc)(PyObject *, PyFrameObject *, int, PyObject *);

/* The following values are used for 'what' for tracefunc functions
 *
 * To add a new kind of trace event, also update "trace_init" in
 * Python/sysmodule.c to define the Python level event name
 */
#define PyTrace_CALL 0
#define PyTrace_EXCEPTION 1
#define PyTrace_LINE 2
#define PyTrace_RETURN 3
#define PyTrace_C_CALL 4
#define PyTrace_C_EXCEPTION 5
#define PyTrace_C_RETURN 6
#define PyTrace_OPCODE 7


typedef struct {
    PyCodeObject *code; // The code object for the bounds. May be NULL.
    PyCodeAddressRange bounds; // Only valid if code != NULL.
} PyTraceInfo;

// Internal structure: you should not use it directly, but use public functions
// like PyThreadState_EnterTracing() and PyThreadState_LeaveTracing().
typedef struct _PyCFrame {
    /* This struct will be threaded through the C stack
     * allowing fast access to per-thread state that needs
     * to be accessed quickly by the interpreter, but can
     * be modified outside of the interpreter.
     *
     * WARNING: This makes data on the C stack accessible from
     * heap objects. Care must be taken to maintain stack
     * discipline and make sure that instances of this struct cannot
     * accessed outside of their lifetime.
     */
    uint8_t use_tracing;  // 0 or 255 (or'ed into opcode, hence 8-bit type)
    /* Pointer to the currently executing frame (it can be NULL) */
    struct _PyInterpreterFrame *current_frame;
    struct _PyCFrame *previous;
} _PyCFrame;

typedef struct _err_stackitem {
    /* This struct represents a single execution context where we might
     * be currently handling an exception.  It is a per-coroutine state
     * (coroutine in the computer science sense, including the thread
     * and generators).
     *
     * This is used as an entry on the exception stack, where each
     * entry indicates if it is currently handling an exception.
     * This ensures that the exception state is not impacted
     * by "yields" from an except handler.  The thread
     * always has an entry (the bottom-most one).
     */

    /* The exception currently being handled in this context, if any. */
    PyObject *exc_value;

    struct _err_stackitem *previous_item;

} _PyErr_StackItem;

typedef struct _stack_chunk {
    struct _stack_chunk *previous;
    size_t size;
    size_t top;
    PyObject * data[1]; /* Variable sized */
} _PyStackChunk;

struct mi_heap_s;
typedef struct mi_heap_s mi_heap_t;
typedef struct _PyEventRc _PyEventRc;

// The PyThreadState typedef is in Include/pystate.h.
struct _ts {
    /* See Python/ceval.c for comments explaining most fields */

    PyThreadState *prev;
    PyThreadState *next;
    PyInterpreterState *interp;

    /* thread status (attached, detached, gc) */
    int status;

    uintptr_t eval_breaker;

    mi_heap_t *heaps;
    mi_heap_t *curheap;

    Py_ssize_t refcount;

    /* Has been initialized to a safe state.

       In order to be effective, this must be set to 0 during or right
       after allocation. */
    int _initialized;

    int py_recursion_remaining;
    int py_recursion_limit;

    int c_recursion_remaining;
    int recursion_headroom; /* Allow 50 more calls to handle any errors. */

    /* 'tracing' keeps track of the execution depth when tracing/profiling.
       This is to prevent the actual trace/profile code from being recorded in
       the trace/profile. */
    int tracing;
    int tracing_what; /* The event currently being traced, if any. */

    /* Pointer to current _PyCFrame in the C stack frame of the currently,
     * or most recently, executing _PyEval_EvalFrameDefault. */
    _PyCFrame *cframe;

    /* The thread will not stop for GC or other stop-the-world requests.
     * Used for *short* critical sections that to prevent deadlocks between
     * finalizers and stopped threads. */
    int cant_stop_wont_stop;

    Py_tracefunc c_profilefunc;
    Py_tracefunc c_tracefunc;
    PyObject *c_profileobj;
    PyObject *c_traceobj;

    /* The exception currently being raised */
    PyObject *curexc_type;
    PyObject *curexc_value;
    PyObject *curexc_traceback;

    /* Pointer to the top of the exception stack for the exceptions
     * we may be currently handling.  (See _PyErr_StackItem above.)
     * This is never NULL. */
    _PyErr_StackItem *exc_info;

    PyObject *dict;  /* Stores per-thread state */

    /* Using a cache is very effective since typically only a single slice is
       created and then deleted again. */
    PySliceObject *slice_cache;

    int gilstate_counter;

    PyObject *async_exc; /* Asynchronous exception to raise */
    unsigned long thread_id; /* Thread id where this tstate was created */

    /* Native thread id where this tstate was created. This will be 0 except on
     * those platforms that have the notion of native thread id, for which the
     * macro PY_HAVE_THREAD_NATIVE_ID is then defined.
     */
    unsigned long native_thread_id;

    uintptr_t fast_thread_id; /* Thread id used for object ownership */

    int trash_delete_nesting;
    PyObject *trash_delete_later;

    uintptr_t critical_section;

    _PyEventRc *done_event; /* Set when thread is about to exit */
    int daemon;

    int coroutine_origin_tracking_depth;

    PyObject *async_gen_firstiter;
    PyObject *async_gen_finalizer;

    PyObject *context;
    uint64_t context_ver;

    Py_ssize_t ref_total;

    /* Unique thread state id. */
    uint64_t id;

    PyTraceInfo trace_info;

    _PyStackChunk *datastack_chunk;
    PyObject **datastack_top;
    PyObject **datastack_limit;

    /* Queue of data pointers to be freed */
    struct _Py_queue_head/*<_PyMemWork>*/ mem_work;

    /* XXX signal handlers should also be here */

    /* The following fields are here to avoid allocation during init.
       The data is exposed through PyThreadState pointer fields.
       These fields should not be accessed directly outside of init.
       This is indicated by an underscore prefix on the field names.

       All other PyInterpreterState pointer fields are populated when
       needed and default to NULL.
       */
       // Note some fields do not have a leading underscore for backward
       // compatibility.  See https://bugs.python.org/issue45953#msg412046.

    /* The thread's exception stack entry.  (Always the last entry.) */
    _PyErr_StackItem exc_state;

    /* The bottom-most frame on the stack. */
    _PyCFrame root_cframe;
};

/* WASI has limited call stack. Python's recursion limit depends on code
   layout, optimization, and WASI runtime. Wasmtime can handle about 700
   recursions, sometimes less. 500 is a more conservative limit. */
#ifndef C_RECURSION_LIMIT
#  ifdef __wasi__
#    define C_RECURSION_LIMIT 500
#  else
#    define C_RECURSION_LIMIT 800
#  endif
#endif

/* other API */

// Alias for backward compatibility with Python 3.8
#define _PyInterpreterState_Get PyInterpreterState_Get

PyAPI_FUNC(PyThreadState *) _PyThreadState_Prealloc(PyInterpreterState *, _PyEventRc *done_event);

/* Similar to PyThreadState_Get(), but don't issue a fatal error
 * if it is NULL. */
PyAPI_FUNC(PyThreadState *) _PyThreadState_UncheckedGet(void);

PyAPI_FUNC(PyObject *) _PyThreadState_GetDict(PyThreadState *tstate);

// Disable tracing and profiling.
PyAPI_FUNC(void) PyThreadState_EnterTracing(PyThreadState *tstate);

// Reset tracing and profiling: enable them if a trace function or a profile
// function is set, otherwise disable them.
PyAPI_FUNC(void) PyThreadState_LeaveTracing(PyThreadState *tstate);

/* PyGILState */

/* Helper/diagnostic function - return 1 if the current thread
   currently holds the GIL, 0 otherwise.

   The function returns 1 if _PyGILState_check_enabled is non-zero. */
PyAPI_FUNC(int) PyGILState_Check(void);

/* Get the single PyInterpreterState used by this process' GILState
   implementation.

   This function doesn't check for error. Return NULL before _PyGILState_Init()
   is called and after _PyGILState_Fini() is called.

   See also _PyInterpreterState_Get() and _PyInterpreterState_GET(). */
PyAPI_FUNC(PyInterpreterState *) _PyGILState_GetInterpreterStateUnsafe(void);

/* The implementation of sys._current_frames()  Returns a dict mapping
   thread id to that thread's current frame.
*/
PyAPI_FUNC(PyObject *) _PyThread_CurrentFrames(void);

/* The implementation of sys._current_exceptions()  Returns a dict mapping
   thread id to that thread's current exception.
*/
PyAPI_FUNC(PyObject *) _PyThread_CurrentExceptions(void);

/* Routines for advanced debuggers, requested by David Beazley.
   Don't use unless you know what you are doing! */
PyAPI_FUNC(PyInterpreterState *) PyInterpreterState_Main(void);
PyAPI_FUNC(PyInterpreterState *) PyInterpreterState_Head(void);
PyAPI_FUNC(PyInterpreterState *) PyInterpreterState_Next(PyInterpreterState *);
PyAPI_FUNC(PyThreadState *) PyInterpreterState_ThreadHead(PyInterpreterState *);
PyAPI_FUNC(PyThreadState *) PyThreadState_Next(PyThreadState *);
PyAPI_FUNC(void) PyThreadState_DeleteCurrent(void);

/* Frame evaluation API */

typedef PyObject* (*_PyFrameEvalFunction)(PyThreadState *tstate, struct _PyInterpreterFrame *, int);

PyAPI_FUNC(_PyFrameEvalFunction) _PyInterpreterState_GetEvalFrameFunc(
    PyInterpreterState *interp);
PyAPI_FUNC(void) _PyInterpreterState_SetEvalFrameFunc(
    PyInterpreterState *interp,
    _PyFrameEvalFunction eval_frame);

PyAPI_FUNC(const PyConfig*) _PyInterpreterState_GetConfig(PyInterpreterState *interp);

/* Get a copy of the current interpreter configuration.

   Return 0 on success. Raise an exception and return -1 on error.

   The caller must initialize 'config', using PyConfig_InitPythonConfig()
   for example.

   Python must be preinitialized to call this method.
   The caller must hold the GIL.

   Once done with the configuration, PyConfig_Clear() must be called to clear
   it. */
PyAPI_FUNC(int) _PyInterpreterState_GetConfigCopy(
    struct PyConfig *config);

/* Set the configuration of the current interpreter.

   This function should be called during or just after the Python
   initialization.

   Update the sys module with the new configuration. If the sys module was
   modified directly after the Python initialization, these changes are lost.

   Some configuration like faulthandler or warnoptions can be updated in the
   configuration, but don't reconfigure Python (don't enable/disable
   faulthandler and don't reconfigure warnings filters).

   Return 0 on success. Raise an exception and return -1 on error.

   The configuration should come from _PyInterpreterState_GetConfigCopy(). */
PyAPI_FUNC(int) _PyInterpreterState_SetConfig(
    const struct PyConfig *config);

// Get the configuration of the current interpreter.
// The caller must hold the GIL.
PyAPI_FUNC(const PyConfig*) _Py_GetConfig(void);


/* cross-interpreter data */

// _PyCrossInterpreterData is similar to Py_buffer as an effectively
// opaque struct that holds data outside the object machinery.  This
// is necessary to pass safely between interpreters in the same process.
typedef struct _xid _PyCrossInterpreterData;

typedef PyObject *(*xid_newobjectfunc)(_PyCrossInterpreterData *);
typedef void (*xid_freefunc)(void *);

struct _xid {
    // data is the cross-interpreter-safe derivation of a Python object
    // (see _PyObject_GetCrossInterpreterData).  It will be NULL if the
    // new_object func (below) encodes the data.
    void *data;
    // obj is the Python object from which the data was derived.  This
    // is non-NULL only if the data remains bound to the object in some
    // way, such that the object must be "released" (via a decref) when
    // the data is released.  In that case the code that sets the field,
    // likely a registered "crossinterpdatafunc", is responsible for
    // ensuring it owns the reference (i.e. incref).
    PyObject *obj;
    // interp is the ID of the owning interpreter of the original
    // object.  It corresponds to the active interpreter when
    // _PyObject_GetCrossInterpreterData() was called.  This should only
    // be set by the cross-interpreter machinery.
    //
    // We use the ID rather than the PyInterpreterState to avoid issues
    // with deleted interpreters.  Note that IDs are never re-used, so
    // each one will always correspond to a specific interpreter
    // (whether still alive or not).
    int64_t interp;
    // new_object is a function that returns a new object in the current
    // interpreter given the data.  The resulting object (a new
    // reference) will be equivalent to the original object.  This field
    // is required.
    xid_newobjectfunc new_object;
    // free is called when the data is released.  If it is NULL then
    // nothing will be done to free the data.  For some types this is
    // okay (e.g. bytes) and for those types this field should be set
    // to NULL.  However, for most the data was allocated just for
    // cross-interpreter use, so it must be freed when
    // _PyCrossInterpreterData_Release is called or the memory will
    // leak.  In that case, at the very least this field should be set
    // to PyMem_RawFree (the default if not explicitly set to NULL).
    // The call will happen with the original interpreter activated.
    xid_freefunc free;
};

PyAPI_FUNC(void) _PyCrossInterpreterData_Init(
        _PyCrossInterpreterData *data,
        PyInterpreterState *interp, void *shared, PyObject *obj,
        xid_newobjectfunc new_object);
PyAPI_FUNC(int) _PyCrossInterpreterData_InitWithSize(
        _PyCrossInterpreterData *,
        PyInterpreterState *interp, const size_t, PyObject *,
        xid_newobjectfunc);
PyAPI_FUNC(void) _PyCrossInterpreterData_Clear(
        PyInterpreterState *, _PyCrossInterpreterData *);

PyAPI_FUNC(int) _PyObject_GetCrossInterpreterData(PyObject *, _PyCrossInterpreterData *);
PyAPI_FUNC(PyObject *) _PyCrossInterpreterData_NewObject(_PyCrossInterpreterData *);
PyAPI_FUNC(int) _PyCrossInterpreterData_Release(_PyCrossInterpreterData *);

PyAPI_FUNC(int) _PyObject_CheckCrossInterpreterData(PyObject *);

/* cross-interpreter data registry */

typedef int (*crossinterpdatafunc)(PyThreadState *tstate, PyObject *,
                                   _PyCrossInterpreterData *);

PyAPI_FUNC(int) _PyCrossInterpreterData_RegisterClass(PyTypeObject *, crossinterpdatafunc);
PyAPI_FUNC(int) _PyCrossInterpreterData_UnregisterClass(PyTypeObject *);
PyAPI_FUNC(crossinterpdatafunc) _PyCrossInterpreterData_Lookup(PyObject *);
