#ifndef Py_INTERNAL_RUNTIME_H
#define Py_INTERNAL_RUNTIME_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_atomic.h"          /* _Py_atomic_address */
#include "pycore_ceval_state.h"     // struct _ceval_runtime_state
#include "pycore_dict_state.h"      // struct _Py_dict_runtime_state
#include "pycore_dtoa.h"            // struct _dtoa_runtime_state
#include "pycore_floatobject.h"     // struct _Py_float_runtime_state
#include "pycore_faulthandler.h"    // struct _faulthandler_runtime_state
#include "pycore_function.h"        // struct _func_runtime_state
#include "pycore_global_objects.h"  // struct _Py_global_objects
#include "pycore_import.h"          // struct _import_runtime_state
#include "pycore_interp.h"          // PyInterpreterState
#include "pycore_parser.h"          // struct _parser_runtime_state
#include "pycore_pymem.h"           // struct _pymem_allocators
#include "pycore_pyhash.h"          // struct pyhash_runtime_state
#include "pycore_pythread.h"        // struct _pythread_runtime_state
#include "pycore_obmalloc.h"        // struct obmalloc_state
#include "pycore_signal.h"          // struct _signals_runtime_state
#include "pycore_time.h"            // struct _time_runtime_state
#include "pycore_tracemalloc.h"     // struct _tracemalloc_runtime_state
#include "pycore_unicodeobject.h"   // struct _Py_unicode_runtime_ids

struct _getargs_runtime_state {
    struct _PyArg_Parser *static_parsers;
};

/* ceval state */

/* GIL state */

struct _gilstate_runtime_state {
    /* bpo-26558: Flag to disable PyGILState_Check().
       If set to non-zero, PyGILState_Check() always return 1. */
    int check_enabled;
    /* The single PyInterpreterState used by this process'
       GILState implementation
    */
    /* TODO: Given interp_main, it may be possible to kill this ref */
    PyInterpreterState *autoInterpreterState;
    Py_tss_t autoTSSkey;
};

/* Runtime audit hook state */

typedef struct _Py_AuditHookEntry {
    struct _Py_AuditHookEntry *next;
    Py_AuditHookFunction hookCFunction;
    void *userData;
} _Py_AuditHookEntry;

/* See pycore_qsbr.h for full definition */
struct qsbr;

/* Full Python runtime state */

/* _PyRuntimeState holds the global state for the CPython runtime.
   That data is exposed in the internal API as a static variable (_PyRuntime).
   */
typedef struct pyruntimestate {
    /* Has been initialized to a safe state.

       In order to be effective, this must be set to 0 during or right
       after allocation. */
    int _initialized;

    /* Is running Py_PreInitialize()? */
    int preinitializing;

    /* Is Python preinitialized? Set to 1 by Py_PreInitialize() */
    int preinitialized;

    /* Is Python core initialized? Set to 1 by _Py_InitializeCore() */
    int core_initialized;

    /* Is Python fully initialized? Set to 1 by Py_Initialize() */
    int initialized;

    /* Has Python started the process of stopping all threads? Protected by HEAD_LOCK() */
    int stop_the_world_requested;

    /* Have all Python threads stopped? */
    int stop_the_world;

    int gc_collecting;

    /* Set by Py_FinalizeEx(). Only reset to NULL if Py_Initialize()
       is called again.

       Use _PyRuntimeState_GetFinalizing() and _PyRuntimeState_SetFinalizing()
       to access it, don't access it directly. */
    _Py_atomic_address _finalizing;

    struct _pymem_allocators allocators;
    struct _obmalloc_state obmalloc;
    struct pyhash_runtime_state pyhash_state;
    struct _time_runtime_state time;
    struct _pythread_runtime_state threads;
    struct _signals_runtime_state signals;

    struct pyinterpreters {
        _PyRawMutex mutex;
        /* The linked list of interpreters, newest first. */
        PyInterpreterState *head;
        /* The runtime's initial interpreter, which has a special role
           in the operation of the runtime.  It is also often the only
           interpreter. */
        PyInterpreterState *main;
        /* next_id is an auto-numbered sequence of small
           integers.  It gets initialized in _PyInterpreterState_Enable(),
           which is called in Py_Initialize(), and used in
           PyInterpreterState_New().  A negative interpreter ID
           indicates an error occurred.  The main interpreter will
           always have an ID of 0.  Overflow results in a RuntimeError.
           If that becomes a problem later then we can adjust, e.g. by
           using a Python int. */
        int64_t next_id;
    } interpreters;
    // XXX Remove this field once we have a tp_* slot.
    struct _xidregistry {
        _PyRawMutex mutex;
        struct _xidregitem *head;
    } xidregistry;

    struct qsbr_shared {
        /* always odd, incremented by two */
        uint64_t s_wr;

        /* Minimum observed read sequence. */
        uint64_t s_rd_seq;

        struct qsbr *head;
        uintptr_t n_free;
    } qsbr_shared;

    unsigned long main_thread;
    PyThreadState *main_tstate;

    PyWideStringList orig_argv;

    struct _parser_runtime_state parser;

#define NEXITFUNCS 32
    void (*exitfuncs[NEXITFUNCS])(void);
    int nexitfuncs;

    struct _import_runtime_state imports;
    struct _ceval_runtime_state ceval;
    struct _gilstate_runtime_state gilstate;
    struct _getargs_runtime_state getargs;
    struct _dtoa_runtime_state dtoa;
    struct _fileutils_state fileutils;
    struct _faulthandler_runtime_state faulthandler;
    struct _tracemalloc_runtime_state tracemalloc;

    PyPreConfig preconfig;

    // Audit values must be preserved when Py_Initialize()/Py_Finalize()
    // is called multiple times.
    Py_OpenCodeHookFunction open_code_hook;
    void *open_code_userdata;
    _Py_AuditHookEntry *audit_hook_head;

    struct _Py_float_runtime_state float_state;
    struct _Py_unicode_runtime_state unicode_state;
    struct _Py_dict_runtime_state dict_state;
    struct _py_func_runtime_state func_state;

    _PyMutex mutex;
    struct {
        /* Used to set PyTypeObject.tp_version_tag */
        // bpo-42745: next_version_tag remains shared by all interpreters
        // because of static types.
        unsigned int next_version_tag;
    } types;

    /* All the objects that are shared by the runtime's interpreters. */
    struct _Py_cached_objects cached_objects;
    struct _Py_static_objects static_objects;

    /* linked list of static code objects */
    struct _Py_llist_node static_code;

    /* */
    struct _Py_queue_head unlinked_code_arrays;

    /* The following fields are here to avoid allocation during init.
       The data is exposed through _PyRuntimeState pointer fields.
       These fields should not be accessed directly outside of init.

       All other _PyRuntimeState pointer fields are populated when
       needed and default to NULL.

       For now there are some exceptions to that rule, which require
       allocation during init.  These will be addressed on a case-by-case
       basis.  Most notably, we don't pre-allocated the several mutex
       (PyThread_type_lock) fields, because on Windows we only ever get
       a pointer type.
       */

    /* PyInterpreterState.interpreters.main */
    PyInterpreterState _main_interpreter;

    _PyMutex stoptheworld_mutex;

    Py_ssize_t ref_total;
} _PyRuntimeState;

#define HEAD_LOCK(runtime) \
    _PyRawMutex_lock(&(runtime)->interpreters.mutex)
#define HEAD_UNLOCK(runtime) \
    _PyRawMutex_unlock(&(runtime)->interpreters.mutex)

/* other API */

PyAPI_DATA(_PyRuntimeState) _PyRuntime;

PyAPI_FUNC(PyStatus) _PyRuntimeState_Init(_PyRuntimeState *runtime);
PyAPI_FUNC(void) _PyRuntimeState_Fini(_PyRuntimeState *runtime);

#ifdef HAVE_FORK
extern PyStatus _PyRuntimeState_ReInitThreads(_PyRuntimeState *runtime);
#endif

PyAPI_FUNC(void) _PyRuntimeState_StopTheWorld(_PyRuntimeState *runtime);
PyAPI_FUNC(void) _PyRuntimeState_StartTheWorld(_PyRuntimeState *runtime);

PyAPI_FUNC(Py_ssize_t) _PyRuntimeState_GetRefTotal(_PyRuntimeState *runtime);

/* Initialize _PyRuntimeState.
   Return NULL on success, or return an error message on failure. */
PyAPI_FUNC(PyStatus) _PyRuntime_Initialize(void);

PyAPI_FUNC(void) _PyRuntime_Finalize(void);


static inline PyThreadState*
_PyRuntimeState_GetFinalizing(_PyRuntimeState *runtime) {
    return (PyThreadState*)_Py_atomic_load_relaxed(&runtime->_finalizing);
}

static inline void
_PyRuntimeState_SetFinalizing(_PyRuntimeState *runtime, PyThreadState *tstate) {
    _Py_atomic_store_relaxed(&runtime->_finalizing, (uintptr_t)tstate);
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_RUNTIME_H */
