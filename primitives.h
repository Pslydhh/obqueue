// This codes is originally from the https://github.com/chaoran/fast-wait-free-queue

/** @file */

#ifndef PRIMITIVES_H
#define PRIMITIVES_H

#if defined(__GNUC__) && __GNUC__ >= 4 && __GNUC_MINOR__ > 7
/**
 * An atomic fetch-and-add.
 */
#define FAA(ptr, val) __atomic_fetch_add(ptr, val, __ATOMIC_RELAXED)
/**
 * An atomic fetch-and-add that also ensures sequential consistency.
 */
#define FAAcs(ptr, val) __atomic_fetch_add(ptr, val, __ATOMIC_SEQ_CST)

/**
 * An atomic compare-and-swap.
 */
#define CAS(ptr, cmp, val) __atomic_compare_exchange_n(ptr, cmp, val, 0, \
    __ATOMIC_RELAXED, __ATOMIC_RELAXED)
/**
 * An atomic compare-and-swap that also ensures sequential consistency.
 */
#define CAScs(ptr, cmp, val) __atomic_compare_exchange_n(ptr, cmp, val, 0, \
    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
/**
 * An atomic compare-and-swap that ensures release semantic when succeed
 * or acquire semantic when failed.
 */
#define CASra(ptr, cmp, val) __atomic_compare_exchange_n(ptr, cmp, val, 0, \
    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)
/**
 * An atomic compare-and-swap that ensures acquire semantic when succeed
 * or relaxed semantic when failed.
 */
#define CASa(ptr, cmp, val) __atomic_compare_exchange_n(ptr, cmp, val, 0, \
    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)

#define XCHG(mem, newvalue) __atomic_exchange_n(mem, newvalue, __ATOMIC_ACQ_REL)

/**
 * An atomic swap.
 */
#define SWAP(ptr, val) __atomic_exchange_n(ptr, val, __ATOMIC_RELAXED)

/**
 * An atomic swap that ensures acquire release semantics.
 */
#define SWAPra(ptr, val) __atomic_exchange_n(ptr, val, __ATOMIC_ACQ_REL)

/**
 * A memory fence to ensure sequential consistency.
 */
#define FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/**
 * An atomic store.
 */
#define STORE(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELAXED)

/**
 * A store with a preceding release fence to ensure all previous load
 * and stores completes before the current store is visiable.
 */
#define RELEASE(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELEASE)

/**
 * A load with a following acquire fence to ensure no following load and
 * stores can start before the current load completes.
 */
#define ACQUIRE(ptr) __atomic_load_n(ptr, __ATOMIC_ACQUIRE)

#else /** Non-GCC or old GCC. */
#if defined(__x86_64__) || defined(_M_X64_)

#define FAA __sync_fetch_and_add
#define FAAcs __sync_fetch_and_add

static inline int
_compare_and_swap(void ** ptr, void ** expected, void * desired) {
  void * oldval = *expected;
  void * newval = __sync_val_compare_and_swap(ptr, oldval, desired);

  if (newval == oldval) {
    return 1;
  } else {
    *expected = newval;
    return 0;
  }
}
#define CAS(ptr, expected, desired) \
  _compare_and_swap((void **) (ptr), (void **) (expected), (void *) (desired))
#define CAScs CAS
#define CASra CAS
#define CASa  CAS

/* 1 if 'type' is a pointer type, 0 otherwise.  */
# define __pointer_type(type) (__builtin_classify_type ((type) 0) == 5)

/* __intptr_t if P is true, or T if P is false.  */
# define __integer_if_pointer_type_sub(T, P) \
  __typeof__ (*(0 ? (__typeof__ (0 ? (T *) 0 : (void *) (P))) 0 \
          : (__typeof__ (0 ? (__intptr_t *) 0 : (void *) (!(P)))) 0))

/* __intptr_t if EXPR has a pointer type, or the type of EXPR otherwise.  */
# define __integer_if_pointer_type(expr) \
  __integer_if_pointer_type_sub(__typeof__ ((__typeof__ (expr)) 0), \
                __pointer_type (__typeof__ (expr)))

/* Cast an integer or a pointer VAL to integer with proper type.  */
# define cast_to_integer(val) ((__integer_if_pointer_type (val)) (val))

#define XCHG(mem, newvalue)\
  ({ __typeof (*mem) result;                              \
     if (sizeof (*mem) == 1)                              \
       __asm __volatile ("xchgb %b0, %1"                      \
             : "=q" (result), "=m" (*mem)                  \
             : "0" (newvalue), "m" (*mem));                  \
     else if (sizeof (*mem) == 2)                          \
       __asm __volatile ("xchgw %w0, %1"                      \
             : "=r" (result), "=m" (*mem)                  \
             : "0" (newvalue), "m" (*mem));                  \
     else if (sizeof (*mem) == 4)                          \
       __asm __volatile ("xchgl %0, %1"                          \
             : "=r" (result), "=m" (*mem)                  \
             : "0" (newvalue), "m" (*mem));                  \
     else                                      \
       __asm __volatile ("xchgq %q0, %1"                      \
             : "=r" (result), "=m" (*mem)                  \
             : "0" ((int64_t) cast_to_integer (newvalue)),     \
               "m" (*mem));                          \
     result; })  
#define SWAP __sync_lock_test_and_set
#define SWAPra SWAP

#define ACQUIRE(p) ({ \
  __typeof__(*(p)) __ret = *p; \
  __asm__("":::"memory"); \
  __ret; \
})

#define RELEASE(p, v) do {\
  __asm__("":::"memory"); \
  *p = v; \
  asm ("sfence" ::: "memory");\
} while (0)
#define FENCE() __sync_synchronize()

#endif
#endif

#if defined(__x86_64__) || defined(_M_X64_)
#define PAUSE() __asm__ ("pause")

static inline
int _CAS2(volatile long * ptr, long * cmp1, long * cmp2, long val1, long val2)
{
  char success;
  long tmp1 = *cmp1;
  long tmp2 = *cmp2;

  __asm__ __volatile__(
      "lock cmpxchg16b %1\n"
      "setz %0"
      : "=q" (success), "+m" (*ptr), "+a" (tmp1), "+d" (tmp2)
      : "b" (val1), "c" (val2)
      : "cc" );

  *cmp1 = tmp1;
  *cmp2 = tmp2;
  return success;
}
#define CAS2(p, o1, o2, n1, n2) \
  _CAS2((volatile long *) p, (long *) o1, (long *) o2, (long) n1, (long) n2)

#define BTAS(ptr, bit) ({ \
  char __ret; \
  __asm__ __volatile__( \
      "lock btsq %2, %0; setnc %1" \
      : "+m" (*ptr), "=r" (__ret) : "ri" (bit) : "cc" ); \
  __ret; \
})

#else
#define PAUSE()
#endif

#endif /* end of include guard: PRIMITIVES_H */

