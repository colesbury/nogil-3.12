#ifndef Py_INTERNAL_PYMEM_H
#define Py_INTERNAL_PYMEM_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pymem.h"      // PyMemAllocatorName


typedef struct {
    /* We tag each block with an API ID in order to tag API violations */
    char api_id;
    PyMemAllocatorEx alloc;
} debug_alloc_api_t;

struct _pymem_allocators {
    struct {
        PyMemAllocatorEx raw;
        PyMemAllocatorEx mem;
        PyMemAllocatorEx obj;
        PyMemAllocatorEx gc;
    } standard;
    struct {
        debug_alloc_api_t raw;
        debug_alloc_api_t mem;
        debug_alloc_api_t obj;
        debug_alloc_api_t gc;
    } debug;
    PyObjectArenaAllocator obj_arena;
};

struct _mem_state {
    _PyMutex mutex;
    /* Queue of data pointers to be freed from dead threads */
    struct _Py_queue_head/*<_PyMemWork>*/ work;
    int nonempty;
};


/* Set the memory allocator of the specified domain to the default.
   Save the old allocator into *old_alloc if it's non-NULL.
   Return on success, or return -1 if the domain is unknown. */
PyAPI_FUNC(int) _PyMem_SetDefaultAllocator(
    PyMemAllocatorDomain domain,
    PyMemAllocatorEx *old_alloc);

/* Special bytes broadcast into debug memory blocks at appropriate times.
   Strings of these are unlikely to be valid addresses, floats, ints or
   7-bit ASCII.

   - PYMEM_CLEANBYTE: clean (newly allocated) memory
   - PYMEM_DEADBYTE dead (newly freed) memory
   - PYMEM_FORBIDDENBYTE: untouchable bytes at each end of a block

   Byte patterns 0xCB, 0xDB and 0xFB have been replaced with 0xCD, 0xDD and
   0xFD to use the same values than Windows CRT debug malloc() and free().
   If modified, _PyMem_IsPtrFreed() should be updated as well. */
#define PYMEM_CLEANBYTE      0xCD
#define PYMEM_DEADBYTE       0xDD
#define PYMEM_FORBIDDENBYTE  0xFD

/* Heuristic checking if a pointer value is newly allocated
   (uninitialized), newly freed or NULL (is equal to zero).

   The pointer is not dereferenced, only the pointer value is checked.

   The heuristic relies on the debug hooks on Python memory allocators which
   fills newly allocated memory with CLEANBYTE (0xCD) and newly freed memory
   with DEADBYTE (0xDD). Detect also "untouchable bytes" marked
   with FORBIDDENBYTE (0xFD). */
static inline int _PyMem_IsPtrFreed(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
#if SIZEOF_VOID_P == 8
    return (value == 0
            || value == (uintptr_t)0xCDCDCDCDCDCDCDCD
            || value == (uintptr_t)0xDDDDDDDDDDDDDDDD
            || value == (uintptr_t)0xFDFDFDFDFDFDFDFD);
#elif SIZEOF_VOID_P == 4
    return (value == 0
            || value == (uintptr_t)0xCDCDCDCD
            || value == (uintptr_t)0xDDDDDDDD
            || value == (uintptr_t)0xFDFDFDFD);
#else
#  error "unknown pointer size"
#endif
}

PyAPI_FUNC(int) _PyMem_GetAllocatorName(
    const char *name,
    PyMemAllocatorName *allocator);

/* Configure the Python memory allocators.
   Pass PYMEM_ALLOCATOR_DEFAULT to use default allocators.
   PYMEM_ALLOCATOR_NOT_SET does nothing. */
PyAPI_FUNC(int) _PyMem_SetupAllocators(PyMemAllocatorName allocator);

/* Free the pointer after all threads are quiescent. */
extern void _PyMem_FreeQsbr(void *ptr);
extern void _PyQsbr_Free(void *ptr, freefunc func);
extern void _PyMem_QsbrPoll(PyThreadState *tstate);
extern void _PyMem_AbandonQsbr(PyThreadState *tstate);
extern void _PyMem_QsbrFini(PyInterpreterState *interp);

extern void * _PyMem_DefaultRawMalloc(size_t);
extern void * _PyMem_DefaultRawCalloc(size_t, size_t);
extern void * _PyMem_DefaultRawRealloc(void *, size_t);
extern void _PyMem_DefaultRawFree(void *);
extern char * _PyMem_DefaultRawStrdup(const char *);
extern wchar_t * _PyMem_DefaultRawWcsdup(const wchar_t *str);

#ifdef __cplusplus
}
#endif
#endif  // !Py_INTERNAL_PYMEM_H
