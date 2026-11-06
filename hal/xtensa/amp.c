/**
 * @file amp.c
 * Message link between the two cores through the shared memory window.
 */

#include <string.h>
#include "amp.h"
#include "rtos_assert.h"

#define AMP_MAGIC   0x414D5031ul        /* "AMP1" */

typedef struct amp_ring
{
    volatile uint16_t head;             /**< Next slot to read (consumer). */
    volatile uint16_t tail;             /**< Next slot to write (producer). */
    amp_msg_t slot[AMP_RING_SLOTS];
} amp_ring_t;

typedef struct amp_shared
{
    volatile uint32_t magic;
    amp_ring_t toCore1;
    amp_ring_t toCore0;
} amp_shared_t;

extern uint8_t _shared_start[];
extern uint8_t _shared_size[];

static amp_shared_t *_pShared;
static amp_ring_t *_pRx, *_pTx;
static rtos_sem_t _semRx = RTOS_SEM_INIT_BINARY(0);

#define CPU_INT_CROSS   13u

static inline void barrier(void)
{
    __asm__ __volatile__ ("memw" : : : "memory");
}

static void crossCoreIsr(void)
{
    rtos_port_ackCrossCore();
    rtos_semGiveFromIsr(&_semRx);
}

void amp_init(void)
{
    RTOS_CHECK((uintptr_t)_shared_size >= sizeof(amp_shared_t), (uintptr_t)_shared_size, sizeof(amp_shared_t));
    _pShared = (amp_shared_t *)_shared_start;
    if(rtos_port_coreId() == 0)
    {
        memset(_pShared, 0, sizeof(*_pShared));
        barrier();
        _pShared->magic = AMP_MAGIC;
        _pRx = &_pShared->toCore0;
        _pTx = &_pShared->toCore1;
        rtos_port_attachInterrupt(CPU_INT_CROSS, RTOS_PORT_SRC_FROM_CPU0, crossCoreIsr);
        RTOS_CHECK(amp_core1_image != NULL  &&  &amp_core1_image_size != NULL, (uintptr_t)amp_core1_image, 0);
        rtos_port_startAppCpu(amp_core1_image, amp_core1_image_size);
    }
    else
    {
        while(_pShared->magic != AMP_MAGIC)
            ;
        _pRx = &_pShared->toCore1;
        _pTx = &_pShared->toCore0;
        rtos_port_attachInterrupt(CPU_INT_CROSS, RTOS_PORT_SRC_FROM_CPU1, crossCoreIsr);
    }
}

bool amp_send(const amp_msg_t *pMsg)
{
    /* The ring is single producer, single consumer between the cores: this core writes
       tail, the other reads it, so no lock is needed across the cores. Between tasks of
       this core the read-modify-write of tail is a critical section, so that two tasks
       calling amp_send at once never take the same slot. */
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    const uint16_t tail = _pTx->tail;
    const uint16_t next = (uint16_t)((tail + 1u) % AMP_RING_SLOTS);

    if(next == _pTx->head)
    {
        rtos_port_exitCritical(cs);
        return false;
    }
    _pTx->slot[tail] = *pMsg;
    barrier();
    _pTx->tail = next;
    rtos_port_exitCritical(cs);
    rtos_port_signalOtherCore();
    return true;
}

uint16_t amp_pending(void)
{
    return (uint16_t)((_pRx->tail + AMP_RING_SLOTS - _pRx->head) % AMP_RING_SLOTS);
}

bool amp_receive(amp_msg_t *pMsg, rtos_tick_t timeout)
{
    /* The timeout covers the whole call: every wake-up that finds the ring empty (the
       other core's interrupt arrived for a message already taken) waits only for what
       is left of it. The read-modify-write of head is a critical section against other
       tasks of this core, as in amp_send. */
    const rtos_tick_t t0 = rtos_getTick();
    for(;;)
    {
        rtos_tick_t left = timeout;
        const rtos_port_cs_t cs = rtos_port_enterCritical();
        const uint16_t head = _pRx->head;
        if(head != _pRx->tail)
        {
            const uint16_t next = (uint16_t)((head + 1u) % AMP_RING_SLOTS);
            barrier();
            *pMsg = _pRx->slot[head];
            barrier();
            _pRx->head = next;
            /* More in the ring: pass the wake-up on to the next receiver. The other core
               signals once per message, but signals that arrive before the interrupt is
               taken merge into one interrupt and one give of the binary semaphore, which
               wakes one receiver; without this a second waiting receiver would sleep
               until the next message or until the first one comes back. The give is a
               no-op (counted as lost) when nobody waits; a receiver woken for a message
               already taken finds the ring empty and waits again. */
            const bool more = next != _pRx->tail;
            rtos_port_exitCritical(cs);
            if(more)
                rtos_semGive(&_semRx);          /* task variant: switches if the woken one is higher */
            return true;
        }
        rtos_port_exitCritical(cs);
        if(timeout != RTOS_FOREVER)
        {
            const rtos_tick_t spent = (rtos_tick_t)(rtos_getTick() - t0);
            if(spent >= timeout)
                return false;
            left = timeout - spent;
        }
        if(!rtos_semTake(&_semRx, left))
            return false;
    }
}
