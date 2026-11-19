/**
 * @file rtos_stream.c
 * Stream buffers (RTOS_CFG_STREAMS): a ring of bytes with writes and reads of any length,
 * one writer (a task or an interrupt) and one reader at a time, a trigger level that wakes
 * the reader once enough has arrived. rtos.h ("Stream buffers") has the contract.
 *
 * The single writer / single reader rule is what the implementation is built on. The
 * writer alone moves the tail and alone touches the free part of the ring; the reader
 * alone moves the head and alone touches the filled part. So the bytes are copied with
 * interrupts enabled, and the critical section covers only the reading of the count that
 * says how much may be copied and the update of count and index afterwards: a bounded
 * handful of instructions whatever the length, which keeps the interrupt latency of the
 * kernel where it is. A reset moves the head up to the tail, so it does not disturb a
 * write that is in progress.
 *
 * The copies are plain loops, not memcpy: a driver that runs this from IRAM on the ESP32
 * must not reach a library function placed in flash (the lesson of nornStore's save).
 */

#include "rtos.h"
#include "rtos_assert.h"
#include "rtos_kernel.h"

#if RTOS_CFG_STREAMS

/** Copy n bytes into the ring at tail, wrapping; returns the new tail. The writer's. */
static uint16_t copyIn(rtos_stream_t *pS, uint16_t tail, const uint8_t *src, uint16_t n)
{
    while(n-- > 0)
    {
        pS->buf[tail] = *src++;
        if(++tail == pS->size)
            tail = 0;
    }
    return tail;
}

/** Copy n bytes out of the ring from head, wrapping; returns the new head. The reader's. */
static uint16_t copyOut(const rtos_stream_t *pS, uint16_t head, uint8_t *dst, uint16_t n)
{
    while(n-- > 0)
    {
        *dst++ = pS->buf[head];
        if(++head == pS->size)
            head = 0;
    }
    return head;
}

/** Account n bytes written; wake the reader if the level is reached. Interrupts disabled.
    Returns true if a task was made ready. */
static inline bool commitWrite(rtos_stream_t *pS, uint16_t tail, uint16_t n)
{
    pS->tail = tail;
    pS->count = (uint16_t)(pS->count + n);
    if(pS->readers != 0  &&  pS->count >= pS->trigger)
    {
        (void)rtos_kernel_releaseHighestWaiter(&pS->readers);
        return true;
    }
    return false;
}

/** Account n bytes read; wake a writer waiting for room. Interrupts disabled. */
static inline bool commitRead(rtos_stream_t *pS, uint16_t head, uint16_t n)
{
    pS->head = head;
    pS->count = (uint16_t)(pS->count - n);
    if(pS->writers != 0)
    {
        (void)rtos_kernel_releaseHighestWaiter(&pS->writers);
        return true;
    }
    return false;
}

/** Ticks left of a timeout started at t0; RTOS_FOREVER stays, 0 when it ran out. */
static rtos_tick_t remaining(rtos_tick_t timeout, rtos_tick_t t0)
{
    rtos_tick_t spent;
    if(timeout == RTOS_FOREVER  ||  timeout == RTOS_NO_WAIT)
        return timeout;
    spent = rtos_tickCount - t0;
    return spent >= timeout ? RTOS_NO_WAIT : (rtos_tick_t)(timeout - spent);
}


void rtos_streamInit(rtos_stream_t *pS, uint8_t *storage, uint16_t size, uint16_t trigger)
{
    RTOS_CHECK(storage != NULL  &&  size != 0, (uintptr_t)storage, size);
    RTOS_CHECK(trigger >= 1  &&  trigger <= size, trigger, size);
    pS->buf = storage;
    pS->size = size;
    pS->head = 0;
    pS->tail = 0;
    pS->count = 0;
    pS->trigger = trigger;
    pS->readers = 0;
    pS->writers = 0;
}


uint16_t rtos_streamWrite(rtos_stream_t *pS, const void *p, uint16_t n, rtos_tick_t timeout)
{
    const uint8_t *src = p;
    const rtos_tick_t t0 = rtos_tickCount;
    uint16_t written = 0;

    while(written < n)
    {
        uint16_t chunk, tail;
        rtos_port_cs_t cs = rtos_port_enterCritical();

        chunk = (uint16_t)(pS->size - pS->count);
        if(chunk == 0)
        {
            const rtos_tick_t left = remaining(timeout, t0);
            if(left == RTOS_NO_WAIT)
            {
                rtos_port_exitCritical(cs);
                break;
            }
            {
                rtos_tcb_t * const pCur = rtos_pCurrent;
                RTOS_CHECK(pS->writers == 0, pCur->prio, pS->writers);   /* one writer */
                pS->writers |= pCur->bit;
                pCur->pWaiters = &pS->writers;
                (void)rtos_kernel_blockOn(RTOS_WAIT_STREAM_WRITE, pS, left);
            }
            rtos_port_exitCritical(cs);
            continue;                       /* room, or the timeout: both show in the next round */
        }
        tail = pS->tail;
        rtos_port_exitCritical(cs);

        if(chunk > (uint16_t)(n - written))
            chunk = (uint16_t)(n - written);
        tail = copyIn(pS, tail, src + written, chunk);
        written = (uint16_t)(written + chunk);

        cs = rtos_port_enterCritical();
        if(commitWrite(pS, tail, chunk))
            rtos_kernel_schedule();
        rtos_port_exitCritical(cs);
    }
    return written;
}


uint16_t rtos_streamWriteFromIsr(rtos_stream_t *pS, const void *p, uint16_t n)
{
    uint16_t chunk = (uint16_t)(pS->size - pS->count);

    if(chunk > n)
        chunk = n;
    if(chunk != 0)
        (void)commitWrite(pS, copyIn(pS, pS->tail, p, chunk), chunk);
    return chunk;
}


uint16_t rtos_streamRead(rtos_stream_t *pS, void *p, uint16_t n, rtos_tick_t timeout)
{
    uint16_t avail, head;
    rtos_port_cs_t cs = rtos_port_enterCritical();

    if(pS->count < pS->trigger  &&  timeout != RTOS_NO_WAIT)
    {
        rtos_tcb_t * const pCur = rtos_pCurrent;
        RTOS_CHECK(pS->readers == 0, pCur->prio, pS->readers);         /* one reader */
        pS->readers |= pCur->bit;
        pCur->pWaiters = &pS->readers;
        (void)rtos_kernel_blockOn(RTOS_WAIT_STREAM_READ, pS, timeout);
        /* Woken at the trigger level, or timed out: either way, take what is there. */
    }
    avail = pS->count < n ? pS->count : n;
    head = pS->head;
    rtos_port_exitCritical(cs);
    if(avail == 0)
        return 0;

    head = copyOut(pS, head, p, avail);

    cs = rtos_port_enterCritical();
    if(commitRead(pS, head, avail))
        rtos_kernel_schedule();
    rtos_port_exitCritical(cs);
    return avail;
}


uint16_t rtos_streamReadFromIsr(rtos_stream_t *pS, void *p, uint16_t n)
{
    const uint16_t avail = pS->count < n ? pS->count : n;

    if(avail != 0)
        (void)commitRead(pS, copyOut(pS, pS->head, p, avail), avail);
    return avail;
}


uint16_t rtos_streamCount(const rtos_stream_t *pS)
{
    return pS->count;
}


uint16_t rtos_streamSpace(const rtos_stream_t *pS)
{
    return (uint16_t)(pS->size - pS->count);
}


void rtos_streamReset(rtos_stream_t *pS)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();

    /* Head up to the tail: the filled part is gone, the writer's side untouched. */
    if(commitRead(pS, pS->tail, pS->count))
        rtos_kernel_schedule();
    rtos_port_exitCritical(cs);
}

#else
typedef int rtos_stream_unused_t;
#endif  /* RTOS_CFG_STREAMS */
