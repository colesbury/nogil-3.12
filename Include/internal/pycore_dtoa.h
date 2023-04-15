#ifndef Py_INTERNAL_DTOA_H
#define Py_INTERNAL_DTOA_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_pymath.h"        // _PY_SHORT_FLOAT_REPR


#if _PY_SHORT_FLOAT_REPR == 1

typedef uint32_t ULong;

struct
Bigint {
    struct Bigint *next;
    int k, maxwds, sign, wds;
    ULong x[1];
};

#ifdef Py_USING_MEMORY_DEBUGGER

struct _dtoa_runtime_state {
    int _not_used;
};
#define _dtoa_runtime_state_INIT {0}

#else  // !Py_USING_MEMORY_DEBUGGER

/* The size of the Bigint freelist */
#define Bigint_Kmax 7

#ifndef PRIVATE_MEM
#define PRIVATE_MEM 2304
#endif
#define Bigint_PREALLOC_SIZE \
    ((PRIVATE_MEM+sizeof(double)-1)/sizeof(double))

struct _dtoa_runtime_state {
    /* p5s is a linked list of powers of 5 of the form 5**(2**i), i >= 2 */
    // XXX This should be freed during runtime fini.
    _PyMutex mutex;
    struct Bigint *p5s;
    struct Bigint *freelist[Bigint_Kmax+1];
    double preallocated[Bigint_PREALLOC_SIZE];
    double *preallocated_next;
};
#define _dtoa_runtime_state_INIT(runtime) \
    { \
        .preallocated_next = runtime.dtoa.preallocated, \
    }

#endif  // !Py_USING_MEMORY_DEBUGGER


/* These functions are used by modules compiled as C extension like math:
   they must be exported. */

PyAPI_FUNC(double) _Py_dg_strtod(const char *str, char **ptr);
PyAPI_FUNC(char *) _Py_dg_dtoa(double d, int mode, int ndigits,
                        int *decpt, int *sign, char **rve);
PyAPI_FUNC(void) _Py_dg_freedtoa(char *s);
PyAPI_FUNC(double) _Py_dg_stdnan(int sign);
PyAPI_FUNC(double) _Py_dg_infinity(int sign);

#endif // _PY_SHORT_FLOAT_REPR == 1

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_DTOA_H */
