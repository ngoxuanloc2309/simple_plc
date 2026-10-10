#include "logger.h"
#include "sx_os_config.h"

/* Single source of truth: the RTOS switch lives in app/sx_os_config.h. */
#define FREE_RTOS SX_OS_USE_FREERTOS

#if FREE_RTOS

#include <FreeRTOS.h>
#include <semphr.h>

static SemaphoreHandle_t logger_mutex = NULL;

/* The mutex exists only after logger_init(); before that (and before the
 * scheduler starts) the logger is used by a single thread, so skipping the
 * lock is safe. Never call the logger from an ISR when FREE_RTOS = 1. */
static inline void logger_lock(void)
{
    if (logger_mutex != NULL) {
        (void)xSemaphoreTake(logger_mutex, portMAX_DELAY);
    }
}

static inline void logger_unlock(void)
{
    if (logger_mutex != NULL) {
        (void)xSemaphoreGive(logger_mutex);
    }
}
#else
#define logger_lock()   ((void)0)
#define logger_unlock() ((void)0)

#endif

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
// #include "fdk_config.h"
#define LOGGER_EN 1
// Global variables to keep track of log files.
static int LOG_LEVEL = LOGGER_INFO;
// static bool write_to_file = false;

static void serial_write_func(const char *s);

static p_log_func serial_write = serial_write_func;

static void serial_write_func(const char *s)
{
    fputs(s, stdout);
}

static const char *log_level_strings[] = {
    "[OFF]",
    "[ERROR]",
    "[WARNING]",
    "[INFO]",
    "[DEBUG]",
    
};
// const char *colors[] = {
//     "\x1B[34m",
//     "\x1B[32m",
//     "\x1B[33m",
//     "\x1B[31m",
//     "\x1B[0m",
// };
const char *colors[] = {
    "\x1B[0m",
    "\x1B[31m",
    "\x1B[33m",
    "\x1B[32m",
    "\x1B[34m", 
    
};
static const char *banner =
    "                                                                         ******               ((((((\r\n"
    "                                                                           *****,.          ######\r\n"
    "                                                                             ,,,,,,,     ,######\r\n"
    " ###########                                                     ##     --     ,,,,,,,   #####\r\n"
    " ##          (##      ## (##########)  ##########  ###########. #####   ##       ,,,,,,,   #\r\n"
    "   -------##   ##   (##  (##       ##  ##.......#. ##       ,##  ##     ##         ,,,,,.\r\n"
    " ###########    (####    (#,       ##  ##       #. #########,##  ##     ##       ,***,..   #\r\n"
    "                 *##                               ##                          ,,,,,,*   (###(\r\n"
    "               ###                                 ##                        ,,,,,,.      ######\r\n"
    "                                                                          ......,,         ######\r\n";

char buff[4096];

void log_func(LOGGING_LEVELS level, const char *TAG, const char *frmt, ...)
{
#if LOGGER_EN
    if (LOG_LEVEL < level)
    {
        return;
    }
    if (serial_write == NULL)
    {
        return;
    }

    logger_lock();

    /* buff is shared by every caller; the lock covers format + write. */
    int n = snprintf(buff, sizeof(buff), "%s%s%s : ", colors[level], log_level_strings[level], TAG);
    if (n < 0 || (size_t)n >= sizeof(buff))
    {
        n = 0;
        buff[0] = '\0';
    }
    va_list argp;
    va_start(argp, frmt);
    vsnprintf(buff + n, sizeof(buff) - (size_t)n, frmt, argp);
    va_end(argp);

    /* Trailer: colour reset + CRLF. Room is guaranteed by the reserve. */
    size_t len = strlen(buff);
    if (len > sizeof(buff) - 16U)
    {
        len = sizeof(buff) - 16U;
    }
    snprintf(buff + len, sizeof(buff) - len, "%s\r\n", colors[LOGGER_OFF]);

    serial_write(buff);

    logger_unlock();
#endif
}
void logger_set_level(LOGGING_LEVELS level)
{
    LOG_LEVEL = level;
}
void log_print_hex(LOGGING_LEVELS level, const char *TAG, uint8_t *hex_buff, uint16_t length)
{
#if LOGGER_EN
    if (LOG_LEVEL < level)
    {
        return;
    }
    if (serial_write == NULL)
    {
        return;
    }

    logger_lock();

    int n = snprintf(buff, sizeof(buff), "%s%s%s : ", colors[level], log_level_strings[level], TAG);
    size_t len = (n < 0 || (size_t)n >= sizeof(buff)) ? 0U : (size_t)n;
    buff[len] = '\0';
    for (uint16_t i = 0; i < length; i++)
    {
        /* 3 chars per byte + CRLF + NUL */
        if (len + 3U + 3U > sizeof(buff))
        {
            break;
        }
        len += (size_t)snprintf(buff + len, sizeof(buff) - len, "%02X ", hex_buff[i]);
    }
    snprintf(buff + len, sizeof(buff) - len, "\r\n");

    serial_write(buff);

    logger_unlock();
#endif
}
void logger_init(LOGGING_LEVELS level, p_log_func p_func)
{
#if FREE_RTOS
    if (logger_mutex == NULL)   /* tolerate a second logger_init() */
    {
        logger_mutex = xSemaphoreCreateMutex();
    }
#endif
    LOG_LEVEL = level;
    if (p_func != NULL)
    {
        serial_write = p_func;
    }
    serial_write(colors[LOGGER_DEBUG]);
    serial_write("\r\n\r\n");
    serial_write(banner);
    serial_write("\r\n\r\n");
    serial_write(colors[LOGGER_INFO]);
}