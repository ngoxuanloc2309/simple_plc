#ifndef LOGGER_H
#define LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef FREE_RTOS
#define FREE_RTOS 0
#endif

typedef enum {
	LOGGER_OFF = 0,
	LOGGER_ERROR,
	LOGGER_WARNING,
	LOGGER_INFO,
	LOGGER_DEBUG,
	
} LOGGING_LEVELS;

typedef void (*p_log_func)(const char *s);

/**
 * @brief Function to initialize the logger.
 *
 * @params[in] level Log level.
 * @params[in] filename Path to the log file.
 * @params[in] to_file Boolean variable to allow writing to file or to stderr stream.
 *
 * @retval None.
 */
void logger_init(LOGGING_LEVELS,p_log_func);

/**
 * @brief Function to write the message to the log file.
 *
 * @params[in] level Log level.
 * @params[in] frmt Format message.
 * @params[in] ... Variable arguments.
 *
 * @retval None.
 */
void log_func(LOGGING_LEVELS level,const char *TAG, const char *frmt, ...);
/**
 * @brief Set the global logging severity threshold.
 *
 * Changes the minimum severity of log messages that will be emitted by the
 * logging system. Any log message with a severity lower than @p level will be
 * suppressed. This affects all subsequent logging calls system-wide.
 *
 * @param level The new logging threshold; must be one of the values from the
 *              LOGGING_LEVELS enumeration (for example: DEBUG, INFO, WARN, ERROR).
 *
 * @note Calling this function with a less restrictive level (e.g., DEBUG)
 *       enables additional, lower-severity output. Calling it with a more
 *       restrictive level (e.g., ERROR) will silence lower-severity output.
 *
 * @threadsafe Implementation is expected to be safe to call from multiple
 *             threads concurrently. If the current implementation is not
 *             thread-safe, synchronize externally (e.g., using a mutex).
 *
 * @since 1.0
 */
void logger_set_level(LOGGING_LEVELS level);
/**
 * @brief Logs a buffer of data in hexadecimal format.
 *
 * This function prints the contents of the provided buffer as a hexadecimal string,
 * using the specified logging level and tag for identification.
 *
 * @param level     The logging level to use (e.g., DEBUG, INFO, ERROR).
 * @param TAG       A string tag to identify the source or context of the log message.
 * @param hex_buff  Pointer to the buffer containing the data to be logged.
 * @param length    The number of bytes in the buffer to log.
 */
void log_print_hex(LOGGING_LEVELS level,const char *TAG,uint8_t *hex_buff,uint16_t length);

/**
 * @brief Logs a debug message with the specified tag and formatted arguments.
 *
 * This macro wraps the log_func function, setting the log level to LOGGER_DEBUG.
 *
 * @param TAG A string literal representing the tag or module name for the log entry.
 * @param ... Additional arguments to be formatted into the log message.
 *
 * @note The variadic arguments are passed directly to log_func.
 */
#define log_debug(TAG,...) log_func(LOGGER_DEBUG,TAG,  __VA_ARGS__)
/**
 * @brief Logs an informational message with the specified tag.
 *
 * This macro wraps the log_func function, automatically setting the log level to LOGGER_INFO.
 * It allows you to log formatted informational messages, similar to printf, with a custom tag.
 *
 * @param TAG A string literal or variable representing the log tag (e.g., module or component name).
 * @param ... Additional arguments to be formatted into the log message.
 *
 * @note The variadic arguments are passed directly to log_func.
 * @example
 *     log_info("NETWORK", "Connected to server: %s", server_address);
 */
#define log_info(TAG,...)  log_func(LOGGER_INFO,TAG,__VA_ARGS__)
/**
 * @brief Logs a warning message with the specified tag and formatted message.
 *
 * This macro wraps the log_func function, setting the log level to LOGGER_WARNING.
 * It allows you to provide a tag and a formatted message (with optional arguments)
 * similar to printf-style formatting.
 *
 * @param TAG A string literal or variable representing the log tag (e.g., module or component name).
 * @param ... Format string followed by optional arguments, as in printf.
 *
 * @note The macro expands to a call to log_func with LOGGER_WARNING as the log level.
 */
#define log_warn(TAG,...)  log_func(LOGGER_WARNING,TAG,__VA_ARGS__)
/**
 * @brief Logs an error message with the specified tag and formatted message.
 *
 * This macro wraps the log_func function, setting the log level to LOGGER_ERROR.
 *
 * @param TAG A string literal representing the tag or module name for the log entry.
 * @param ... The format string and additional arguments, similar to printf.
 *
 * @note Usage example:
 *       log_error("NETWORK", "Failed to connect: %d", error_code);
 */
#define log_error(TAG,...) log_func(LOGGER_ERROR,TAG,__VA_ARGS__)


#ifdef __cplusplus
}
#endif

#endif