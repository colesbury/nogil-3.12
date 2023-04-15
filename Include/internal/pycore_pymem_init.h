#ifndef Py_INTERNAL_PYMEM_INIT_H
#define Py_INTERNAL_PYMEM_INIT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_pymem.h"


/********************************/
/* the allocators' initializers */

extern void * _PyMem_RawMalloc(void *, size_t);
extern void * _PyMem_RawCalloc(void *, size_t, size_t);
extern void * _PyMem_RawRealloc(void *, void *, size_t);
extern void _PyMem_RawFree(void *, void *);
#define PYRAW_ALLOC {NULL, _PyMem_RawMalloc, _PyMem_RawCalloc, _PyMem_RawRealloc, _PyMem_RawFree}

extern void* _PyMem_Malloc(void *, size_t);
extern void* _PyMem_Calloc(void *, size_t, size_t);
extern void* _PyMem_Realloc(void *, void *, size_t);
extern void _PyMem_Free(void *, void *);

extern void* _PyObject_Malloc(void *, size_t);
extern void* _PyObject_Calloc(void *, size_t, size_t);
extern void* _PyObject_Realloc(void *, void *, size_t);

extern void* _PyGC_Malloc(void *, size_t);
extern void* _PyGC_Calloc(void *, size_t, size_t);
extern void* _PyGC_Realloc(void *, void *, size_t);

#  define PYMEM_ALLOC {NULL, _PyMem_Malloc, _PyMem_Calloc, _PyMem_Realloc, _PyMem_Free}
#  define PYOBJ_ALLOC {NULL, _PyObject_Malloc, _PyObject_Calloc, _PyObject_Realloc, _PyMem_Free}
#  define PYGC_ALLOC {NULL, _PyGC_Malloc, _PyGC_Calloc, _PyGC_Realloc, _PyMem_Free}

extern void* _PyMem_DebugRawMalloc(void *, size_t);
extern void* _PyMem_DebugRawCalloc(void *, size_t, size_t);
extern void* _PyMem_DebugRawRealloc(void *, void *, size_t);
extern void _PyMem_DebugRawFree(void *, void *);

extern void* _PyMem_DebugMalloc(void *, size_t);
extern void* _PyMem_DebugCalloc(void *, size_t, size_t);
extern void* _PyMem_DebugRealloc(void *, void *, size_t);
extern void _PyMem_DebugFree(void *, void *);

#define PYDBGRAW_ALLOC(runtime) \
    {&(runtime).allocators.debug.raw, _PyMem_DebugRawMalloc, _PyMem_DebugRawCalloc, _PyMem_DebugRawRealloc, _PyMem_DebugRawFree}
#define PYDBGMEM_ALLOC(runtime) \
    {&(runtime).allocators.debug.mem, _PyMem_DebugMalloc, _PyMem_DebugCalloc, _PyMem_DebugRealloc, _PyMem_DebugFree}
#define PYDBGOBJ_ALLOC(runtime) \
    {&(runtime).allocators.debug.obj, _PyMem_DebugMalloc, _PyMem_DebugCalloc, _PyMem_DebugRealloc, _PyMem_DebugFree}
#define PYDBGGC_ALLOC(runtime) \
    {&(runtime).allocators.debug.gc, _PyMem_DebugMalloc, _PyMem_DebugCalloc, _PyMem_DebugRealloc, _PyMem_DebugFree}

extern void * _PyMem_ArenaAlloc(void *, size_t);
extern void _PyMem_ArenaFree(void *, void *, size_t);

#ifdef Py_DEBUG
# define _pymem_allocators_standard_INIT(runtime) \
    { \
        PYDBGRAW_ALLOC(runtime), \
        PYDBGMEM_ALLOC(runtime), \
        PYDBGOBJ_ALLOC(runtime), \
        PYDBGGC_ALLOC(runtime), \
    }
#else
# define _pymem_allocators_standard_INIT(runtime) \
    { \
        PYRAW_ALLOC, \
        PYMEM_ALLOC, \
        PYOBJ_ALLOC, \
        PYGC_ALLOC, \
    }
#endif

#define _pymem_allocators_debug_INIT \
   { \
       {'r', PYRAW_ALLOC}, \
       {'m', PYMEM_ALLOC}, \
       {'o', PYOBJ_ALLOC}, \
       {'g', PYGC_ALLOC}, \
   }

#  define _pymem_allocators_obj_arena_INIT \
    { NULL, _PyMem_ArenaAlloc, _PyMem_ArenaFree }


#ifdef __cplusplus
}
#endif
#endif  // !Py_INTERNAL_PYMEM_INIT_H
