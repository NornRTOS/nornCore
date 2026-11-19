/**
 * @file test_log.c
 * Host unit test of hal/log.h on the host console (hal/host/uart.c captures what is
 * written). Covered: the prefix with class letter and time, the default level INFO, a
 * level change at run time (INFO off under WARNING, back on under INFO), PANIC written
 * whatever the level, DEBUG compiled in here (LOG_CFG_MAX raised) and shown only under
 * LOG_DEBUG, LOG_ON, the one string forms, log_levelName, the time in the prefix
 * following the tick, a module's own level through LOG_LEVEL_VAR with log_setLevel over
 * the module table, and the line lock (LOG_CFG_LOCK 1 in test_log/rtos_config.h: every
 * line here takes and gives it; a second line would block for ever if it leaked).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_CFG_MAX LOG_DEBUG
#include "rtos.h"
#include "log.h"

static unsigned _checks, _fails;
static char _cap[256];

#define CHECK(cond)  check((cond), #cond, __LINE__)
static void check(bool ok, const char *what, int line)
{
    ++ _checks;
    if(!ok)
    {
        ++ _fails;
        printf("CHECK FAILED line %d: %s\n", line, what);
    }
}

static void begin(void)
{
    uart_hostCapture(_cap, sizeof _cap);
}

static bool is(const char *expected)
{
    const bool ok = strcmp(_cap, expected) == 0;
    if(!ok)
        printf("  got      \"%s\"\n  expected \"%s\"\n", _cap, expected);
    return ok;
}

void test_idleHook(void)
{
}

int main(void)
{
    uint32_t i;

    rtos_init();        /* tick 0, nothing running: the tests call the macros directly */

    CHECK(log_level == LOG_INFO);

    begin(); LOG_I(uart_puts("hello ")); uart_putU32(0); /* nothing after end */
    CHECK(strncmp(_cap, "I (0) hello \r\n", 14) == 0);

    begin(); LOG_I(uart_puts("a "); uart_putU32(12); uart_puts(" b"));
    CHECK(is("I (0) a 12 b\r\n"));
    begin(); LOG_E(uart_puts("e")); CHECK(is("E (0) e\r\n"));
    begin(); LOG_W(uart_puts("w")); CHECK(is("W (0) w\r\n"));
    begin(); LOG_D(uart_puts("d")); CHECK(is(""));                   /* above INFO */
    begin(); LOG_P(uart_puts("p")); CHECK(is("P (0) p\r\n"));
    CHECK(LOG_ON(LOG_INFO)); CHECK(!LOG_ON(LOG_DEBUG));

    log_level = LOG_WARNING;
    begin(); LOG_I(uart_puts("i")); CHECK(is(""));
    begin(); LOGS_W("w"); CHECK(is("W (0) w\r\n"));
    begin(); LOGS_E("e"); CHECK(is("E (0) e\r\n"));
    CHECK(!LOG_ON(LOG_INFO)); CHECK(LOG_ON(LOG_WARNING));

    log_level = LOG_PANIC;
    begin(); LOGS_E("e"); CHECK(is(""));
    begin(); LOGS_P("p"); CHECK(is("P (0) p\r\n"));             /* always */

    log_level = LOG_DEBUG;
    begin(); LOGS_D("d"); CHECK(is("D (0) d\r\n"));
    begin(); LOGS_I("i"); CHECK(is("I (0) i\r\n"));

    /* the time follows the tick */
    for(i = 0; i < 1234; ++i)
        rtos_host_tick();
    begin(); LOGS_I("t"); CHECK(is("I (1234) t\r\n"));

    /* the names */
    CHECK(strcmp(log_levelName(LOG_PANIC), "panic") == 0);
    CHECK(strcmp(log_levelName(LOG_ERROR), "error") == 0);
    CHECK(strcmp(log_levelName(LOG_WARNING), "warning") == 0);
    CHECK(strcmp(log_levelName(LOG_INFO), "info") == 0);
    CHECK(strcmp(log_levelName(LOG_DEBUG), "debug") == 0);
    CHECK(strcmp(log_levelName(9), "?") == 0);
    begin(); log_begin(9); log_end(); CHECK(is("D (1234) \r\n"));   /* clamped letter */

    /* capture overflow: the count goes on, the buffer stays terminated */
    uart_hostCapture(_cap, 4);
    uart_puts("abcdef");
    CHECK(strcmp(_cap, "abc") == 0  &&  uart_hostCaptured() == 6);
    uart_hostCapture(NULL, 0);

    /* --- a module's own level: the macros test LOG_LEVEL_VAR, here redirected --- */
    {
        static LOG_MODULE_LEVEL(tst);                       /* uint8_t tst_log_level = LOG_INFO */
        static const log_module_t mods[] = { { "tst", &tst_log_level } };
        uint8_t n;
        CHECK(log_modules(&n) == NULL  &&  n == 0);
        log_setModules(mods, 1);
        CHECK(log_modules(&n) == mods  &&  n == 1);
        log_level = LOG_INFO;
#undef LOG_LEVEL_VAR
#define LOG_LEVEL_VAR tst_log_level
        tst_log_level = LOG_WARNING;
        begin(); LOGS_I("quiet module"); CHECK(is(""));             /* the module is at warning */
        begin(); LOGS_W("module warning"); CHECK(strstr(_cap, "module warning") != NULL);
        CHECK(log_level == LOG_INFO);                               /* the global one untouched */
        log_setLevel(LOG_DEBUG);                                    /* both */
        CHECK(log_level == LOG_DEBUG  &&  tst_log_level == LOG_DEBUG);
        begin(); LOGS_D("module debug"); CHECK(strstr(_cap, "module debug") != NULL);
#undef LOG_LEVEL_VAR
#define LOG_LEVEL_VAR log_level
        log_setLevel(LOG_INFO);
        CHECK(tst_log_level == LOG_INFO);
        log_setModules(NULL, 0);
    }

    /* --- LOG_AT_ISR and LOG_P write without the lock; a second locked line follows --- */
    begin(); LOG_AT_ISR(LOG_INFO, uart_puts("isr")); CHECK(strstr(_cap, "isr\r\n") != NULL);
    begin(); LOGS_I("after"); CHECK(strstr(_cap, "after\r\n") != NULL);

    printf("%s: test_log (%u checks, %u failures)\n", _fails == 0 ? "PASS" : "FAIL", _checks, _fails);
    return _fails == 0 ? 0 : 1;
}
