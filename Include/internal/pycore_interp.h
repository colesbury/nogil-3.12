#ifndef Py_INTERNAL_INTERP_H
#define Py_INTERNAL_INTERP_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include <stdbool.h>

#include "pycore_atomic.h"        // _Py_atomic_address
#include "pycore_ast_state.h"     // struct ast_state
#include "pycore_ceval_state.h"   // struct _ceval_state
#include "pycore_code.h"          // struct callable_cache
#include "pycore_context.h"       // struct _Py_context_state
#include "pycore_dict_state.h"    // struct _Py_dict_state
#include "pycore_exceptions.h"    // struct _Py_exc_state
#include "pycore_floatobject.h"   // struct _Py_float_state
#include "pycore_function.h"      // FUNC_MAX_WATCHERS
#include "pycore_genobject.h"     // struct _Py_async_gen_state
#include "pycore_gc.h"            // struct _gc_runtime_state
#include "pycore_list.h"          // struct _Py_list_state
#include "pycore_llist.h"         // struct llist_node
#include "pycore_mrocache.h"      // struct _mro_cache_state
#include "pycore_global_objects.h"  // struct _Py_interp_static_objects
#include "pycore_pymem.h"         // struct _mem_work
#include "pycore_tuple.h"         // struct _Py_tuple_state
#include "pycore_typeobject.h"    // struct type_cache
#include "pycore_unicodeobject.h" // struct _Py_unicode_state
#include "pycore_warnings.h"      // struct _warnings_runtime_state


// atexit state
typedef struct {
    PyObject *func;
    PyObject *args;
    PyObject *kwargs;
} atexit_callback;

struct atexit_state {
    atexit_callback **callbacks;
    int ncallbacks;
    int callback_len;
};


struct _Py_long_state {
    int max_str_digits;
};

/* Defined in pycore_refcnt.h */
typedef struct _PyObjectQueue _PyObjectQueue;

/* Biased reference counting per-thread state */
struct brc_state {
    /* linked-list of thread states per hash bucket */
    struct llist_node bucket_node;

    /* queue of objects to be merged (protected by bucket mutex) */
    _PyObjectQueue *queue;

    /* local queue of objects to be merged */
    _PyObjectQueue *local_queue;
};

typedef struct PyThreadStateImpl {
    // semi-public fields are in PyThreadState
    PyThreadState tstate;

    struct _Py_float_state float_state;
    struct _Py_tuple_state tuple;
    struct _Py_list_state list;
    struct _Py_dict_thread_state dict_state;
    struct _Py_async_gen_state async_gen;
    struct _Py_context_state context;

    struct brc_state brc;

    struct qsbr *qsbr;

    _PyObjectQueue *cached_queue;
} PyThreadStateImpl;


/* interpreter state */

/* PyInterpreterState holds the global state for one of the runtime's
   interpreters.  Typically the initial (main) interpreter is the only one.

   The PyInterpreterState typedef is in Include/pytypedefs.h.
   */
struct _is {

    PyInterpreterState *next;

    struct pythreads {
        uint64_t next_unique_id;
        /* The linked list of threads, newest first. */
        PyThreadState *head;
        /* Used in Modules/_threadmodule.c. */
        Py_ssize_t count;
        /* Support for runtime thread stack size tuning.
           A value of 0 means using the platform's default stack size
           or the size specified by the THREAD_STACK_SIZE macro. */
        /* Used in Python/thread.c. */
        size_t stacksize;
    } threads;

    /* Reference to the _PyRuntime global variable. This field exists
       to not have to pass runtime in addition to tstate to a function.
       Get runtime from tstate: tstate->interp->runtime. */
    struct pyruntimestate *runtime;

    int64_t id;
    int64_t id_refcount;
    int requires_idref;
    PyThread_type_lock id_mutex;

    /* Has been initialized to a safe state.

       In order to be effective, this must be set to 0 during or right
       after allocation. */
    int _initialized;
    int finalizing;

    struct _ceval_state ceval;
    struct _gc_runtime_state gc;
    struct _mem_state mem;
    struct _mro_cache_state mro_cache;

    // sys.modules dictionary
    PyObject *modules;
    /* This is the list of module objects for all legacy (single-phase init)
       extension modules ever loaded in this process (i.e. imported
       in this interpreter or in any other).  Py_None stands in for
       modules that haven't actually been imported in this interpreter.

       A module's index (PyModuleDef.m_base.m_index) is used to look up
       the corresponding module object for this interpreter, if any.
       (See PyState_FindModule().)  When any extension module
       is initialized during import, its moduledef gets initialized by
       PyModuleDef_Init(), and the first time that happens for each
       PyModuleDef, its index gets set to the current value of
       a global counter (see _PyRuntimeState.imports.last_module_index).
       The entry for that index in this interpreter remains unset until
       the module is actually imported here.  (Py_None is used as
       a placeholder.)  Note that multi-phase init modules always get
       an index for which there will never be a module set.

       This is initialized lazily in _PyState_AddModule(), which is also
       where modules get added. */
    PyObject *modules_by_index;
    // Dictionary of the sys module
    PyObject *sysdict;
    // Dictionary of the builtins module
    PyObject *builtins;
    // importlib module
    PyObject *importlib;
    // override for config->use_frozen_modules (for tests)
    // (-1: "off", 1: "on", 0: no override)
    int override_frozen_modules;

    PyObject *codec_search_path;
    PyObject *codec_search_cache;
    PyObject *codec_error_registry;
    int codecs_initialized;

    PyConfig config;
#ifdef HAVE_DLOPEN
    int dlopenflags;
#endif
    unsigned long feature_flags;

    PyObject *dict;  /* Stores per-interpreter state */

    PyObject *builtins_copy;
    PyObject *import_func;
    // Initialized to _PyEval_EvalFrameDefault().
    _PyFrameEvalFunction eval_frame;

    PyFunction_WatchCallback func_watchers[FUNC_MAX_WATCHERS];
    // One bit is set for each non-NULL entry in func_watchers
    uint8_t active_func_watchers;

    Py_ssize_t co_extra_user_count;
    freefunc co_extra_freefuncs[MAX_CO_EXTRA_USERS];

#ifdef HAVE_FORK
    PyObject *before_forkers;
    PyObject *after_forkers_parent;
    PyObject *after_forkers_child;
#endif

    struct _warnings_runtime_state warnings;
    struct atexit_state atexit;

    PyObject *audit_hooks;
    PyType_WatchCallback type_watchers[TYPE_MAX_WATCHERS];
    PyCode_WatchCallback code_watchers[CODE_MAX_WATCHERS];
    // One bit is set for each non-NULL entry in code_watchers
    uint8_t active_code_watchers;

    struct _Py_unicode_state unicode;
    struct _Py_long_state long_state;

    struct _Py_dict_state dict_state;
    struct _Py_exc_state exc_state;

    struct ast_state ast;
    struct types_state types;
    struct callable_cache callable_cache;
    PyCodeObject *interpreter_trampoline;

    struct _Py_queue_head mro_buckets_to_free;

    struct _Py_interp_cached_objects cached_objects;
    struct _Py_interp_static_objects static_objects;

    /* The following fields are here to avoid allocation during init.
       The data is exposed through PyInterpreterState pointer fields.
       These fields should not be accessed directly outside of init.

       All other PyInterpreterState pointer fields are populated when
       needed and default to NULL.

       For now there are some exceptions to that rule, which require
       allocation during init.  These will be addressed on a case-by-case
       basis.  Also see _PyRuntimeState regarding the various mutex fields.
       */

    /* the initial PyInterpreterState.threads.head */
    PyThreadStateImpl _initial_thread;
};


/* other API */

extern void _PyInterpreterState_ClearModules(PyInterpreterState *interp);
extern void _PyInterpreterState_Clear(PyThreadState *tstate);


/* cross-interpreter data registry */

/* For now we use a global registry of shareable classes.  An
   alternative would be to add a tp_* slot for a class's
   crossinterpdatafunc. It would be simpler and more efficient. */

struct _xidregitem;

struct _xidregitem {
    struct _xidregitem *prev;
    struct _xidregitem *next;
    PyObject *cls;  // weakref to a PyTypeObject
    crossinterpdatafunc getdata;
};

PyAPI_FUNC(PyInterpreterState*) _PyInterpreterState_LookUpID(int64_t);

PyAPI_FUNC(int) _PyInterpreterState_IDInitref(PyInterpreterState *);
PyAPI_FUNC(int) _PyInterpreterState_IDIncref(PyInterpreterState *);
PyAPI_FUNC(void) _PyInterpreterState_IDDecref(PyInterpreterState *);
PyAPI_FUNC(void) _PyInterpreterState_WaitForThreads(PyInterpreterState *);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_INTERP_H */
