#ifndef SPLC_LOG_H
#define SPLC_LOG_H

/*
 * splc_log.h - the only logging interface SimplePLC code uses.
 *
 *     log_error(TAG, fmt, ...)    log_warn(...)    log_info(...)    log_debug(...)
 *
 * Same idea as lwIP's LWIP_PLATFORM_DIAG: the library does not decide where
 * log text goes. Each call ends in one macro,
 *
 *     SPLC_LOG_OUTPUT(level, tag, fmt, ...)       level: SPLC_LOG_LVL_xxx (1..4)
 *
 * that the PRODUCT can define in splcopts.h to route SimplePLC's log into the
 * host firmware's own logger (or printf, or nothing). If the product does not
 * define it, the default sends everything to log_func() of "logger.h":
 *
 *   - SPLC_LOGGER_BUILTIN = 1 (default): that logger.h/logger.c is SimplePLC's
 *     own (utils/logger), initialised by the board file (logger_init()).
 *   - SPLC_LOGGER_BUILTIN = 0: SimplePLC does not build its own logger; the
 *     host firmware supplies "logger.h" (e.g. GatewayV2 libs/logger) and
 *     calls logger_init() itself.
 *
 * Example, host firmware with its own LOGx macros, in splcopts.h:
 *
 *     #include "my_log.h"
 *     #define SPLC_LOG_OUTPUT(level, tag, ...)  MY_LOG((level), tag, __VA_ARGS__)
 *
 * (the header the macro needs must be included from splcopts.h itself).
 * A definition expanding to nothing turns SimplePLC's log off.
 */

#include "splc_opt.h"

#define SPLC_LOG_LVL_ERROR   1
#define SPLC_LOG_LVL_WARN    2
#define SPLC_LOG_LVL_INFO    3
#define SPLC_LOG_LVL_DEBUG   4

#ifndef SPLC_LOG_OUTPUT
#include "logger.h"
/* Values match LOGGER_ERROR..LOGGER_DEBUG of logger.h. */
#define SPLC_LOG_OUTPUT(level, tag, ...) \
    log_func((LOGGING_LEVELS)(level), (tag), __VA_ARGS__)
#endif

#define log_error(TAG, ...)  SPLC_LOG_OUTPUT(SPLC_LOG_LVL_ERROR, TAG, __VA_ARGS__)
#define log_warn(TAG, ...)   SPLC_LOG_OUTPUT(SPLC_LOG_LVL_WARN,  TAG, __VA_ARGS__)
#define log_info(TAG, ...)   SPLC_LOG_OUTPUT(SPLC_LOG_LVL_INFO,  TAG, __VA_ARGS__)
#define log_debug(TAG, ...)  SPLC_LOG_OUTPUT(SPLC_LOG_LVL_DEBUG, TAG, __VA_ARGS__)

#endif /* SPLC_LOG_H */