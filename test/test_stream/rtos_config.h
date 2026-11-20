#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/* Kernel configuration of test_stream: three tasks (writer, reader, a second waiter), the
   stream buffers on. */

#define RTOS_CFG_NUM_TASKS      3
#define RTOS_CFG_STACK_CHECK    0
#define RTOS_CFG_STREAMS        1
#define RTOS_CFG_IDLE_HOOK      test_idleHook

void test_idleHook(void);

#endif  /* RTOS_CONFIG_INCLUDED */
