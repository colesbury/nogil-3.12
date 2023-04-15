#ifndef Py_INTERNAL_TYPEOBJECT_H
#define Py_INTERNAL_TYPEOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif


/* runtime lifecycle */

extern PyStatus _PyTypes_InitTypes(PyInterpreterState *);
extern void _PyTypes_FiniTypes(PyInterpreterState *);
extern void _PyTypes_Fini(PyInterpreterState *);


/* other API */

/* Length of array of slotdef pointers used to store slots with the
   same __name__.  There should be at most MAX_EQUIV-1 slotdef entries with
   the same __name__, for any __name__. Since that's a static property, it is
   appropriate to declare fixed-size arrays for this. */
#define MAX_EQUIV 10

typedef struct wrapperbase pytype_slotdef;

/* For now we hard-code this to a value for which we are confident
   all the static builtin types will fit (for all builds). */
#define _Py_MAX_STATIC_BUILTIN_TYPES 200

typedef struct {
    PyTypeObject *type;
    PyObject *tp_subclasses;
    /* We never clean up weakrefs for static builtin types since
       they will effectively never get triggered.  However, there
       are also some diagnostic uses for the list of weakrefs,
       so we still keep it. */
    PyWeakrefControl *tp_weaklist;
} static_builtin_state;

static inline PyWeakrefControl **
_PyStaticType_GET_WEAKREFS_LISTPTR(static_builtin_state *state)
{
    assert(state != NULL);
    return &state->tp_weaklist;
}

struct types_state {
    size_t num_builtins_initialized;
    static_builtin_state builtins[_Py_MAX_STATIC_BUILTIN_TYPES];
};


extern int _PyStaticType_InitBuiltin(PyTypeObject *type);
extern static_builtin_state * _PyStaticType_GetState(PyTypeObject *);
extern void _PyStaticType_ClearWeakRefs(PyTypeObject *type);
extern void _PyStaticType_Dealloc(PyTypeObject *type);

PyObject *
_Py_type_getattro_impl(PyTypeObject *type, PyObject *name, int *suppress_missing_attribute);
PyObject *
_Py_type_getattro(PyTypeObject *type, PyObject *name);

PyObject *_Py_slot_tp_getattro(PyObject *self, PyObject *name);
PyObject *_Py_slot_tp_getattr_hook(PyObject *self, PyObject *name);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_TYPEOBJECT_H */
