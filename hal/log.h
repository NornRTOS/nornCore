#ifndef LOG_INCLUDED
#define LOG_INCLUDED
/**
 * @file log.h
 * Log classes over the console UART: PANIC, ERROR, WARNING, INFO and DEBUG, a run time
 * level that says which of them are written, and a compile time cap that keeps the ones
 * nobody will ever want out of the image. Part of the HAL, not of the kernel: the kernel
 * never logs, and an application that does not use these macros links none of this.
 *
 *     LOG_I(uart_puts("address "); uart_puts(nn_ip4ToStr(s, addr)));
 *     LOGS_W("server not reachable");                  one string, in program memory
 *
 * writes `I (12345) address 10.0.0.5` when INFO is on: the letter of the class, the time
 * in milliseconds since start, the text, CRLF. The statements inside LOG_x() are those
 * of the uart driver (uart_puts, uart_putsP, uart_putU32, ...) and run only when the
 * class is on, so a message that is off costs one byte compare and no UART time.
 *
 * The level at run time is `log_level`, LOG_INFO by default: PANIC, ERROR, WARNING and
 * INFO are written, DEBUG is not. The application sets it (`log_level = LOG_WARNING;`),
 * nornSHELL's `log` command sets it from the console, and an application with nornStore
 * keeps it in its configuration (examples/device in nornRTOS: `config set system.loglvl`).
 * PANIC is written whatever the level.
 *
 * LOG_CFG_MAX (default LOG_INFO) is the compile time cap: a message above it is not in
 * the image at all, text included, so DEBUG messages cost nothing until an application
 * asks for them with -DLOG_CFG_MAX=LOG_DEBUG (and then shows them with log_level).
 * LOG_CFG_TIME 0 leaves the time out of the prefix (`I address 10.0.0.5`).
 *
 * A level per module: a file that defines LOG_MODULE before including this header tests
 * that module's own level instead of log_level, so one module can be quietened while the
 * rest keeps talking. The module defines the variable once with LOG_MODULE_LEVEL(name)
 * and declares it for others with LOG_MODULE_EXTERN(name) in its header:
 *
 *     #define LOG_MODULE nw                      at the top of the module's files
 *     #include "log.h"
 *     LOG_MODULE_LEVEL(nw);                      once: uint8_t nw_log_level = LOG_INFO
 *
 * log_setLevel() sets log_level and every module in the table the application has given
 * with log_setModules() (a name and a pointer per module), and nornSHELL's `log` shows
 * and sets them by name (`log wifi warning`). A module level is a plain variable too.
 * Cost: a module's test reads its own byte instead of the shared one, nothing else.
 *
 * The UART is not locked by default: lines from two tasks can interleave, as with
 * uart_puts. LOG_CFG_LOCK 1 serializes whole lines with a binary semaphore (one object,
 * a take and a give per line; no priority ceiling, so a task waiting to log waits for the
 * line in progress, nobody else is affected). With the lock on, LOG_E..LOG_D are for task
 * context (also before rtos_start); an interrupt or a kernel hook logs with LOG_AT_ISR(),
 * which never locks, and LOG_P never locks either, since a panic must get out.
 */

#include <stdint.h>
#include "uart.h"

#ifndef LOG_CFG_LOCK
# define LOG_CFG_LOCK   0
#endif

#define LOG_PANIC       0u
#define LOG_ERROR       1u
#define LOG_WARNING     2u
#define LOG_INFO        3u
#define LOG_DEBUG       4u

#ifndef LOG_CFG_MAX
# define LOG_CFG_MAX    LOG_INFO
#endif
#ifndef LOG_CFG_TIME
# define LOG_CFG_TIME   1
#endif

/** The classes written: those with a level up to and including this one. LOG_INFO at
    start. Plain variable: read and set it directly, or log_setLevel() for this and the
    modules' levels together. */
extern uint8_t log_level;

/** The level variable the macros in this file test: the module's with LOG_MODULE
    defined, log_level otherwise. */
#define LOG_CAT_(a, b)          a##b
#define LOG_CAT(a, b)           LOG_CAT_(a, b)
#ifdef LOG_MODULE
# define LOG_LEVEL_VAR          LOG_CAT(LOG_MODULE, _log_level)
extern uint8_t LOG_LEVEL_VAR;
#else
# define LOG_LEVEL_VAR          log_level
#endif

/** Defines a module's level variable (once, in one of its files), and declares it. */
#define LOG_MODULE_LEVEL(mod)   uint8_t LOG_CAT(mod, _log_level) = LOG_INFO
#define LOG_MODULE_EXTERN(mod)  extern uint8_t LOG_CAT(mod, _log_level)

/** A module as the application lists it for log_setModules(): the name `log` takes
    ("wifi") and the module's level variable. */
typedef struct log_module
{
    const char *name;
    uint8_t *pLevel;
} log_module_t;

/** The application's table of modules with their own level, for log_setLevel() and the
    `log` command; the table must stay valid. Without a call there are none. */
void log_setModules(const log_module_t *pModules, uint8_t n);

/** The table given, \a pCount filled with its length; NULL and 0 if none. */
const log_module_t *log_modules(uint8_t *pCount);

/** Sets log_level and every module's level to \a level. */
void log_setLevel(uint8_t level);

/** True if a message of \a level (LOG_ERROR .. LOG_DEBUG) would be written now. With a
    constant level the compile time cap folds the whole test and what it guards away. */
#define LOG_ON(level)   ((level) <= LOG_CFG_MAX  &&  (level) <= LOG_LEVEL_VAR)

/** The statements in the argument list, between the prefix and the end of line, when
    \a level (LOG_ERROR .. LOG_DEBUG) is on. */
#define LOG_AT(level, ...)                                                      \
    do { if(LOG_ON(level)) { log_begin(level); __VA_ARGS__; log_end(); } } while(0)

/** As LOG_AT without the line lock (LOG_CFG_LOCK): for interrupt context and kernel
    hooks, where blocking is not possible. The same as LOG_AT with the lock off. */
#define LOG_AT_ISR(level, ...)                                                  \
    do { if(LOG_ON(level)) { log_prefix(level); __VA_ARGS__; log_eol(); } } while(0)

#define LOG_P(...)      do { log_prefix(LOG_PANIC); __VA_ARGS__; log_eol(); } while(0)
#define LOG_E(...)      LOG_AT(LOG_ERROR, __VA_ARGS__)
#define LOG_W(...)      LOG_AT(LOG_WARNING, __VA_ARGS__)
#define LOG_I(...)      LOG_AT(LOG_INFO, __VA_ARGS__)
#define LOG_D(...)      LOG_AT(LOG_DEBUG, __VA_ARGS__)

/** One string literal, kept in program memory on the AVR. */
#define LOGS_P(text)    LOG_P(uart_putsP(HAL_FSTR(text)))
#define LOGS_E(text)    LOG_E(uart_putsP(HAL_FSTR(text)))
#define LOGS_W(text)    LOG_W(uart_putsP(HAL_FSTR(text)))
#define LOGS_I(text)    LOG_I(uart_putsP(HAL_FSTR(text)))
#define LOGS_D(text)    LOG_D(uart_putsP(HAL_FSTR(text)))

/** The prefix of a line: the class letter (P E W I D) and, with LOG_CFG_TIME, the time
    in ms in parentheses, then a space. Code that builds a line in several steps calls
    log_begin() and log_end() like the macros do; with LOG_CFG_LOCK they also take and
    give the line lock, log_prefix() and log_eol() never do. */
void log_prefix(uint8_t level);

/** The end of a line: CRLF. */
void log_eol(void);

#if LOG_CFG_LOCK
void log_begin(uint8_t level);
void log_end(void);
#else
# define log_begin(level)   log_prefix(level)
# define log_end()          log_eol()
#endif

/** The name of a class in lower case ("panic" .. "debug"; "?" beyond LOG_DEBUG), a
    program memory string: print it with uart_putsP(), read it with hal_flashReadByte(). */
const char *log_levelName(uint8_t level);

#endif  /* LOG_INCLUDED */
