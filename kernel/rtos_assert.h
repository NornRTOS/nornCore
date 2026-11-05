#ifndef RTOS_ASSERT_INCLUDED
#define RTOS_ASSERT_INCLUDED
/**
 * @file rtos_assert.h
 * The kernel's two kinds of checks, kept apart on purpose:
 *
 *   RTOS_ASSERT(cond)             a contract between the application and the kernel, or
 *                                 an internal invariant: a mutex unlocked by a task that
 *                                 does not own it, a wait list that does not contain the
 *                                 task being removed. Compiled with RTOS_CFG_ASSERT; a
 *                                 failure calls rtos_assertFailed (default: rtos_panic
 *                                 with RTOS_PANIC_ASSERT, file and line).
 *   RTOS_CHECK(cond, arg1, arg2)  a mandatory check that is compiled in every
 *                                 configuration, because continuing would corrupt memory
 *                                 or dereference nothing: a task id used as an array
 *                                 index, a stack too small to hold the port's frames, a
 *                                 NULL storage or callback, a block handed back that is
 *                                 not the pool's or the heap's, a memory layout the port
 *                                 relies on. A failure is rtos_panic with
 *                                 RTOS_PANIC_ARGUMENT and the two arguments.
 *
 * Both are cheap (a compare and a branch on the failure side); the difference is that an
 * application may turn the contracts off when it trusts itself, never the checks.
 * Panics proper (rtos_panic from the kernel's own detectors: stack overflow, watchdog,
 * deadlock, configuration mismatch) are the third kind: a state in which running on is
 * no longer safe.
 */

#include "rtos_config.h"
#include "rtos.h"

#if RTOS_CFG_ASSERT
# define RTOS_ASSERT(cond)                                                      \
    do {                                                                        \
        if(!(cond))                                                             \
            rtos_assertFailed(RTOS_PORT_STR(__FILE__), (uint16_t)__LINE__);     \
    } while(0)
#else
# define RTOS_ASSERT(cond)  do { } while(0)
#endif

#define RTOS_CHECK(cond, arg1, arg2)                                            \
    do {                                                                        \
        if(!(cond))                                                             \
            rtos_panic(RTOS_PANIC_ARGUMENT, (uint32_t)(arg1), (uint32_t)(arg2));\
    } while(0)

#endif  /* RTOS_ASSERT_INCLUDED */
