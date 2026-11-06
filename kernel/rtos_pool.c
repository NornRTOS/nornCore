/**
 * @file rtos_pool.c
 * Fixed size block memory pool. The free blocks form a singly linked list threaded
 * through the blocks themselves, so allocation and release are O(1) and the pool needs no
 * bookkeeping memory beyond the blocks. A blocking allocation hands the released block
 * straight to the highest priority waiter (see rtos_queue.c for the rationale).
 *   The link is read and written with memcpy: the storage is a byte array of any
 * alignment and the block size any value from sizeof(void *) up, so a pointer access
 * through the block would be misaligned (a fault on the Xtensa, undefined behaviour
 * everywhere). memcpy of a pointer compiles to the same load or store when the compiler
 * can prove alignment and to byte accesses otherwise.
 */

#include <string.h>
#include "rtos.h"
#include "rtos_assert.h"
#include "rtos_kernel.h"


/*
 * Local functions
 */

/** Take a block from the free list. Interrupts disabled. NULL if empty. */
static inline void *take(rtos_pool_t *pPool)
{
    void * const pBlock = pPool->pFree;

    if(pBlock != NULL)
    {
        memcpy(&pPool->pFree, pBlock, sizeof(void *));
        -- pPool->numFree;
        if(pPool->numFree < pPool->minFree)
            pPool->minFree = pPool->numFree;
    }
    return pBlock;
}


/*
 * Public API
 */

void rtos_poolInit(rtos_pool_t *pPool, uint8_t *storage, uint16_t blockSize, uint8_t numBlocks)
{
    uint8_t i;
    uint8_t *p = storage;

    RTOS_CHECK(storage != NULL  &&  blockSize >= sizeof(void *)  &&  numBlocks > 0, (uintptr_t)storage, blockSize);
    pPool->storage = storage;
    pPool->blockSize = blockSize;
    pPool->numBlocks = numBlocks;
    pPool->numFree = numBlocks;
    pPool->waiters = 0;
    pPool->pFree = storage;
    for(i=1; i<numBlocks; ++i)
    {
        void * const pNext = p + blockSize;
        memcpy(p, &pNext, sizeof(void *));
        p = pNext;
    }
    {
        void * const pNone = NULL;
        memcpy(p, &pNone, sizeof(void *));
    }
    pPool->minFree = numBlocks;
    pPool->failed = 0;
}


void *rtos_poolAlloc(rtos_pool_t *pPool, rtos_tick_t timeout)
{
    void *pBlock;
    const rtos_port_cs_t cs = rtos_port_enterCritical();

    pBlock = take(pPool);
    if(pBlock == NULL  &&  timeout != RTOS_NO_WAIT)
    {
        rtos_tcb_t * const pCur = rtos_pCurrent;

        /* The releasing task writes the block address into pBlock through pXfer. */
        pCur->pXfer = &pBlock;
        pPool->waiters |= pCur->bit;
        pCur->pWaiters = &pPool->waiters;
        if(rtos_kernel_blockOn(RTOS_WAIT_POOL, pPool, timeout))
            pBlock = NULL;
    }
    if(pBlock == NULL  &&  pPool->failed < 0xFFFFu)
        ++ pPool->failed;
    rtos_port_exitCritical(cs);
    return pBlock;
}


void *rtos_poolAllocFromIsr(rtos_pool_t *pPool)
{
    return take(pPool);
}


void rtos_poolFreeFromIsr(rtos_pool_t *pPool, void *pBlock)
{
    /* The block must be one of the pool's: inside the storage and on a block boundary.
       The offset is kept in size_t: a pool may exceed 64 KiB where the RAM allows. */
    RTOS_CHECK((uint8_t *)pBlock >= pPool->storage
                &&  (size_t)((uint8_t *)pBlock - pPool->storage) < (size_t)pPool->blockSize * pPool->numBlocks
                &&  (size_t)((uint8_t *)pBlock - pPool->storage) % pPool->blockSize == 0
               , (uintptr_t)pBlock, (uintptr_t)pPool->storage);
    if(pPool->waiters != 0)
    {
        *(void **)rtos_kernel_releaseHighestWaiter(&pPool->waiters)->pXfer = pBlock;
    }
    else
    {
        RTOS_CHECK(pPool->numFree < pPool->numBlocks, (uintptr_t)pBlock, pPool->numFree);   /* freed twice */
        memcpy(pBlock, &pPool->pFree, sizeof(void *));
        pPool->pFree = pBlock;
        ++ pPool->numFree;
    }
}


void rtos_poolFree(rtos_pool_t *pPool, void *pBlock)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();

    rtos_poolFreeFromIsr(pPool, pBlock);
    rtos_kernel_schedule();
    rtos_port_exitCritical(cs);
}


uint8_t rtos_poolFreeCount(const rtos_pool_t *pPool)
{
    return pPool->numFree;
}
