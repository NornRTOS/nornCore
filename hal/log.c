/**
 * @file log.c
 * The run time level, the module table, the prefix of a log line and the optional line
 * lock; the macros are in log.h. Nothing here is referenced unless the application logs,
 * so the linker drops it otherwise, and each function and variable sits in its own
 * section so that what is not called goes too.
 */

#include "log.h"
#include "rtos.h"

uint8_t log_level = LOG_INFO;

static const log_module_t *_pModules;
static uint8_t _nModules;

void log_setModules(const log_module_t *pModules, uint8_t n)
{
    _pModules = pModules;
    _nModules = n;
}

const log_module_t *log_modules(uint8_t *pCount)
{
    *pCount = _nModules;
    return _pModules;
}

void log_setLevel(uint8_t level)
{
    uint8_t i;
    log_level = level;
    for(i = 0; i < _nModules; ++i)
        *_pModules[i].pLevel = level;
}

#if LOG_CFG_LOCK
/* One line at a time: taken in log_begin for every class but PANIC, given in log_end.
   A binary semaphore, not a mutex: no ceiling, so the task writing the line keeps its
   own priority and only another logger waits for it. */
static rtos_sem_t _lineSem = RTOS_SEM_INIT_BINARY(1);

void log_begin(uint8_t level)
{
    if(level != LOG_PANIC)
        (void)rtos_semTake(&_lineSem, RTOS_FOREVER);
    log_prefix(level);
}

void log_end(void)
{
    log_eol();
    rtos_semGive(&_lineSem);
}
#endif

static const char _letters[] HAL_PROGMEM = "PEWID";
static const char _nPanic[]   HAL_PROGMEM = "panic";
static const char _nError[]   HAL_PROGMEM = "error";
static const char _nWarning[] HAL_PROGMEM = "warning";
static const char _nInfo[]    HAL_PROGMEM = "info";
static const char _nDebug[]   HAL_PROGMEM = "debug";
static const char _nNone[]    HAL_PROGMEM = "?";

void log_prefix(uint8_t level)
{
    uart_putc(hal_flashReadByte(&_letters[level <= LOG_DEBUG ? level : LOG_DEBUG]));
#if LOG_CFG_TIME
    {
        const uint32_t t = (uint32_t)rtos_getTick();
#if RTOS_CFG_TICK_HZ == 1000
        const uint32_t ms = t;
#else
        /* in two steps so that a long run does not overflow the product */
        const uint32_t ms = t / (uint32_t)RTOS_CFG_TICK_HZ * 1000ul
                            + (t % (uint32_t)RTOS_CFG_TICK_HZ) * 1000ul / (uint32_t)RTOS_CFG_TICK_HZ;
#endif
        uart_putc(' ');
        uart_putc('(');
        uart_putU32(ms);
        uart_putc(')');
    }
#endif
    uart_putc(' ');
}

void log_eol(void)
{
    uart_putc('\r');
    uart_putc('\n');
}

const char *log_levelName(uint8_t level)
{
    switch(level)
    {
    case LOG_PANIC:   return _nPanic;
    case LOG_ERROR:   return _nError;
    case LOG_WARNING: return _nWarning;
    case LOG_INFO:    return _nInfo;
    case LOG_DEBUG:   return _nDebug;
    default:          return _nNone;
    }
}
