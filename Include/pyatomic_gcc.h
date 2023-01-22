#ifndef Py_ATOMIC_GCC_H
#  error "this header file must not be included directly"
#endif

static inline int
_Py_atomic_add_int(volatile int *address, int value)
{
    return __atomic_fetch_add(address, value, __ATOMIC_SEQ_CST);
}

static inline int32_t
_Py_atomic_add_int32(volatile int32_t *address, int32_t value)
{
    return __atomic_fetch_add(address, value, __ATOMIC_SEQ_CST);
}

static inline int64_t
_Py_atomic_add_int64(volatile int64_t *address, int64_t value)
{
    return __atomic_fetch_add(address, value, __ATOMIC_SEQ_CST);
}

static inline intptr_t
_Py_atomic_add_intptr(volatile intptr_t *address, intptr_t value)
{
    return __atomic_fetch_add(address, value, __ATOMIC_SEQ_CST);
}

static inline uint32_t
_Py_atomic_add_uint32(volatile uint32_t *address, uint32_t value)
{
    return __atomic_fetch_add(address, value, __ATOMIC_SEQ_CST);
}

static inline uint64_t
_Py_atomic_add_uint64(volatile uint64_t *address, uint64_t value)
{
    return __atomic_fetch_add(address, value, __ATOMIC_SEQ_CST);
}

static inline uintptr_t
_Py_atomic_add_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    return __atomic_fetch_add(address, value, __ATOMIC_SEQ_CST);
}

static inline Py_ssize_t
_Py_atomic_add_ssize(volatile Py_ssize_t *address, Py_ssize_t value)
{
    return __atomic_fetch_add(address, value, __ATOMIC_SEQ_CST);
}


static inline int
_Py_atomic_compare_exchange_int(volatile int *address, int expected, int value)
{
    return __atomic_compare_exchange_n(address, &expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline int
_Py_atomic_compare_exchange_int32(volatile int32_t *address, int32_t expected, int32_t value)
{
    return __atomic_compare_exchange_n(address, &expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline int
_Py_atomic_compare_exchange_int64(volatile int64_t *address, int64_t expected, int64_t value)
{
    return __atomic_compare_exchange_n(address, &expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline int
_Py_atomic_compare_exchange_intptr(volatile intptr_t *address, intptr_t expected, intptr_t value)
{
    return __atomic_compare_exchange_n(address, &expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline int
_Py_atomic_compare_exchange_uint32(volatile uint32_t *address, uint32_t expected, uint32_t value)
{
    return __atomic_compare_exchange_n(address, &expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline int
_Py_atomic_compare_exchange_uint64(volatile uint64_t *address, uint64_t expected, uint64_t value)
{
    return __atomic_compare_exchange_n(address, &expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline int
_Py_atomic_compare_exchange_uintptr(volatile uintptr_t *address, uintptr_t expected, uintptr_t value)
{
    return __atomic_compare_exchange_n(address, &expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline int
_Py_atomic_compare_exchange_ptr(volatile void *address, void *expected, void *value)
{
    volatile void *e = expected;
    return __atomic_compare_exchange_n((volatile void **)address, &e, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}


static inline int
_Py_atomic_exchange_int(volatile int *address, int value)
{
    return __atomic_exchange_n(address, value, __ATOMIC_SEQ_CST);
}

static inline int32_t
_Py_atomic_exchange_int32(volatile int32_t *address, int32_t value)
{
    return __atomic_exchange_n(address, value, __ATOMIC_SEQ_CST);
}

static inline int64_t
_Py_atomic_exchange_int64(volatile int64_t *address, int64_t value)
{
    return __atomic_exchange_n(address, value, __ATOMIC_SEQ_CST);
}

static inline intptr_t
_Py_atomic_exchange_intptr(volatile intptr_t *address, intptr_t value)
{
    return __atomic_exchange_n(address, value, __ATOMIC_SEQ_CST);
}

static inline uint32_t
_Py_atomic_exchange_uint32(volatile uint32_t *address, uint32_t value)
{
    return __atomic_exchange_n(address, value, __ATOMIC_SEQ_CST);
}

static inline uint64_t
_Py_atomic_exchange_uint64(volatile uint64_t *address, uint64_t value)
{
    return __atomic_exchange_n(address, value, __ATOMIC_SEQ_CST);
}

static inline uintptr_t
_Py_atomic_exchange_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    return __atomic_exchange_n(address, value, __ATOMIC_SEQ_CST);
}

static inline void *
_Py_atomic_exchange_ptr(volatile void *address, void *value)
{
    return __atomic_exchange_n((void **)address, value, __ATOMIC_SEQ_CST);
}

static inline uint32_t
_Py_atomic_and_uint32(volatile uint32_t *address, uint32_t value)
{
    return __atomic_fetch_and(address, value, __ATOMIC_SEQ_CST);
}

static inline uint64_t
_Py_atomic_and_uint64(volatile uint64_t *address, uint64_t value)
{
    return __atomic_fetch_and(address, value, __ATOMIC_SEQ_CST);
}

static inline uintptr_t
_Py_atomic_and_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    return __atomic_fetch_and(address, value, __ATOMIC_SEQ_CST);
}

static inline uint32_t
_Py_atomic_or_uint32(volatile uint32_t *address, uint32_t value)
{
    return __atomic_fetch_or(address, value, __ATOMIC_SEQ_CST);
}

static inline uint64_t
_Py_atomic_or_uint64(volatile uint64_t *address, uint64_t value)
{
    return __atomic_fetch_or(address, value, __ATOMIC_SEQ_CST);
}

static inline uintptr_t
_Py_atomic_or_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    return __atomic_fetch_or(address, value, __ATOMIC_SEQ_CST);
}

static inline int
_Py_atomic_load_int(const volatile int *address)
{
    return __atomic_load_n(address, __ATOMIC_SEQ_CST);
}

static inline int32_t
_Py_atomic_load_int32(const volatile int32_t *address)
{
    return __atomic_load_n(address, __ATOMIC_SEQ_CST);
}

static inline int64_t
_Py_atomic_load_int64(const volatile int64_t *address)
{
    return __atomic_load_n(address, __ATOMIC_SEQ_CST);
}

static inline intptr_t
_Py_atomic_load_intptr(const volatile intptr_t *address)
{
    return __atomic_load_n(address, __ATOMIC_SEQ_CST);
}

static inline uint32_t
_Py_atomic_load_uint32(const volatile uint32_t *address)
{
    return __atomic_load_n(address, __ATOMIC_SEQ_CST);
}

static inline uint64_t
_Py_atomic_load_uint64(const volatile uint64_t *address)
{
    return __atomic_load_n(address, __ATOMIC_SEQ_CST);
}

static inline uintptr_t
_Py_atomic_load_uintptr(const volatile uintptr_t *address)
{
    return __atomic_load_n(address, __ATOMIC_SEQ_CST);
}

static inline Py_ssize_t
_Py_atomic_load_ssize(const volatile Py_ssize_t *address)
{
    return __atomic_load_n(address, __ATOMIC_SEQ_CST);
}

static inline void *
_Py_atomic_load_ptr(const volatile void *address)
{
    return (void *)__atomic_load_n((volatile void **)address, __ATOMIC_SEQ_CST);
}


static inline int
_Py_atomic_load_int_relaxed(const volatile int *address)
{
    return __atomic_load_n(address, __ATOMIC_RELAXED);
}

static inline int32_t
_Py_atomic_load_int32_relaxed(const volatile int32_t *address)
{
    return __atomic_load_n(address, __ATOMIC_RELAXED);
}

static inline int64_t
_Py_atomic_load_int64_relaxed(const volatile int64_t *address)
{
    return __atomic_load_n(address, __ATOMIC_RELAXED);
}

static inline intptr_t
_Py_atomic_load_intptr_relaxed(const volatile intptr_t *address)
{
    return __atomic_load_n(address, __ATOMIC_RELAXED);
}

static inline uint32_t
_Py_atomic_load_uint32_relaxed(const volatile uint32_t *address)
{
    return __atomic_load_n(address, __ATOMIC_RELAXED);
}

static inline uint64_t
_Py_atomic_load_uint64_relaxed(const volatile uint64_t *address)
{
    return __atomic_load_n(address, __ATOMIC_RELAXED);
}

static inline uintptr_t
_Py_atomic_load_uintptr_relaxed(const volatile uintptr_t *address)
{
    return __atomic_load_n(address, __ATOMIC_RELAXED);
}

static inline Py_ssize_t
_Py_atomic_load_ssize_relaxed(const volatile Py_ssize_t *address)
{
    return __atomic_load_n(address, __ATOMIC_RELAXED);
}

static inline void *
_Py_atomic_load_ptr_relaxed(const volatile void *address)
{
     return (void *)__atomic_load_n((const volatile void **)address, __ATOMIC_RELAXED);
}

static inline void
_Py_atomic_store_int(volatile int *address, int value)
{
    __atomic_store_n(address, value, __ATOMIC_SEQ_CST);
}

static inline void
_Py_atomic_store_int32(volatile int32_t *address, int32_t value)
{
    __atomic_store_n(address, value, __ATOMIC_SEQ_CST);
}

static inline void
_Py_atomic_store_int64(volatile int64_t *address, int64_t value)
{
    __atomic_store_n(address, value, __ATOMIC_SEQ_CST);
}

static inline void
_Py_atomic_store_intptr(volatile intptr_t *address, intptr_t value)
{
    __atomic_store_n(address, value, __ATOMIC_SEQ_CST);
}

static inline void
_Py_atomic_store_uint32(volatile uint32_t *address, uint32_t value)
{
    __atomic_store_n(address, value, __ATOMIC_SEQ_CST);
}

static inline void
_Py_atomic_store_uint64(volatile uint64_t *address, uint64_t value)
{
    __atomic_store_n(address, value, __ATOMIC_SEQ_CST);
}

static inline void
_Py_atomic_store_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    __atomic_store_n(address, value, __ATOMIC_SEQ_CST);
}

static inline void
_Py_atomic_store_ptr(volatile void *address, void *value)
{
    __atomic_store_n((volatile void **)address, value, __ATOMIC_SEQ_CST);
}

static inline void
_Py_atomic_store_ssize(volatile Py_ssize_t *address, Py_ssize_t value)
{
    __atomic_store_n(address, value, __ATOMIC_SEQ_CST);
}

static inline void
_Py_atomic_store_int_relaxed(volatile int *address, int value)
{
    __atomic_store_n(address, value, __ATOMIC_RELAXED);
}

static inline void
_Py_atomic_store_int32_relaxed(volatile int32_t *address, int32_t value)
{
    __atomic_store_n(address, value, __ATOMIC_RELAXED);
}

static inline void
_Py_atomic_store_int64_relaxed(volatile int64_t *address, int64_t value)
{
    __atomic_store_n(address, value, __ATOMIC_RELAXED);
}

static inline void
_Py_atomic_store_intptr_relaxed(volatile intptr_t *address, intptr_t value)
{
    __atomic_store_n(address, value, __ATOMIC_RELAXED);
}

static inline void
_Py_atomic_store_uint32_relaxed(volatile uint32_t *address, uint32_t value)
{
    __atomic_store_n(address, value, __ATOMIC_RELAXED);
}

static inline void
_Py_atomic_store_uint64_relaxed(volatile uint64_t *address, uint64_t value)
{
    __atomic_store_n(address, value, __ATOMIC_RELAXED);
}

static inline void
_Py_atomic_store_uint64_release(volatile uint64_t *address, uint64_t value)
{
    __atomic_store_n(address, value, __ATOMIC_RELEASE);
}

static inline void
_Py_atomic_store_uintptr_relaxed(volatile uintptr_t *address, uintptr_t value)
{
    __atomic_store_n(address, value, __ATOMIC_RELAXED);
}

static inline void
_Py_atomic_store_ptr_relaxed(volatile void *address, void *value)
{
    __atomic_store_n((volatile void **)address, value, __ATOMIC_RELAXED);
}

static inline void
_Py_atomic_store_ptr_release(volatile void *address, void *value)
{
    __atomic_store_n((volatile void **)address, value, __ATOMIC_RELEASE);
}

static inline void
_Py_atomic_store_ssize_relaxed(volatile Py_ssize_t *address, Py_ssize_t value)
{
    __atomic_store_n(address, value, __ATOMIC_RELAXED);
}

static inline void
_Py_atomic_store_uint8_relaxed(volatile uint8_t *address, uint8_t value)
{
    __atomic_store_n(address, value, __ATOMIC_RELAXED);
}

 static inline void
_Py_atomic_fence_seq_cst(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

 static inline void
_Py_atomic_fence_release(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}
