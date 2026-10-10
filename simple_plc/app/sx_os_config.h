#ifndef SX_OS_CONFIG_H
#define SX_OS_CONFIG_H

/*
 * OS integration switch for the whole library.
 *
 *   SX_OS_USE_FREERTOS = 0 (default)  bare-metal: the host calls
 *                                     plc_engine_init() once and
 *                                     plc_engine_poll() from its main loop.
 *   SX_OS_USE_FREERTOS = 1            the host runs both from ONE FreeRTOS
 *                                     task. The library creates no task of
 *                                     its own; it only
 *                                       - sleeps through the scheduler instead of
 *                                         busy-waiting (sx_delay_ms(), USB waits),
 *                                       - makes the logger thread-safe.
 *
 * Can be overridden from the compiler command line (-DSX_OS_USE_FREERTOS=1).
 * Requirements when it is 1 are listed in components/os/sx_os.h.
 */
#ifndef SX_OS_USE_FREERTOS
#define SX_OS_USE_FREERTOS  0
#endif

/* Derived, never set by hand: the two cannot disagree. */
#if SX_OS_USE_FREERTOS
#define SX_NO_OS            0
#else
#define SX_NO_OS            1
#endif

#endif /* SX_OS_CONFIG_H */